#include "slowcontrol/periodic_poller.h"
#include "common/logger.h"
#include "modbus/board_client.h"
#include "modbus/modbus_rtu.h"

#include <chrono>

namespace danss {

namespace {
uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count());
}
} // namespace

const char* to_string(BoardCommand::Type t) {
    switch (t) {
        case BoardCommand::Type::WRITE_SETPOINTS: return "write setpoints";
        case BoardCommand::Type::SAVE_TO_FLASH: return "save to flash";
        case BoardCommand::Type::LOAD_FROM_FLASH: return "load from flash";
    }
    return "?";
}

struct PeriodicPoller::Branch {
    BranchConfig config;
    std::thread thread;
    std::unique_ptr<ModbusRTU> bus;
    mutable std::mutex stats_mutex;
    BranchStats stats{};
    uint64_t cycles = 0;
    std::deque<BoardCommand> queue;  // под mutex_ поллера
};

PeriodicPoller::PeriodicPoller(const SystemConfig& config, BoardRegistry& registry)
    : config_(config), registry_(registry), interval_s_(config.poll_interval_sec > 0 ? config.poll_interval_sec : 300) {
    for (const auto& bc : config_.branches) {
        auto b = std::make_unique<Branch>();
        b->config = bc;
        branches_.push_back(std::move(b));
    }
}

PeriodicPoller::~PeriodicPoller() {
    stop();
}

void PeriodicPoller::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
    }
    for (auto& b : branches_) {
        Branch* raw = b.get();
        raw->thread = std::thread([this, raw] { run_branch(*raw); });
    }
    LOG_INFO("PeriodicPoller: started " + std::to_string(branches_.size()) + " branch thread(s), interval "
             + std::to_string(static_cast<int>(interval_s_.load())) + " s");
}

void PeriodicPoller::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& b : branches_)
        if (b->thread.joinable()) b->thread.join();
    // Невыполненные команды — отказ, чтобы ожидающий не завис
    for (auto& b : branches_) {
        std::deque<BoardCommand> left;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            left.swap(b->queue);
        }
        for (auto& c : left)
            if (c.done) c.done(false, "poller stopped");
    }
}

void PeriodicPoller::poll_now() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_seq_++;
    }
    cv_.notify_all();
}

bool PeriodicPoller::submit(uint8_t branch_id, BoardCommand cmd) {
    for (auto& b : branches_) {
        if (b->config.id != branch_id) continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return false;
            b->queue.push_back(std::move(cmd));
        }
        cv_.notify_all();
        return true;
    }
    return false;
}

BranchStats PeriodicPoller::branch_stats(uint8_t id) const {
    for (const auto& b : branches_)
        if (b->config.id == id) {
            std::lock_guard<std::mutex> lock(b->stats_mutex);
            return b->stats;
        }
    return BranchStats{};
}

uint64_t PeriodicPoller::cycles(uint8_t id) const {
    for (const auto& b : branches_)
        if (b->config.id == id) {
            std::lock_guard<std::mutex> lock(b->stats_mutex);
            return b->cycles;
        }
    return 0;
}

bool PeriodicPoller::ensure_bus(Branch& b) {
    if (b.bus && b.bus->is_open()) return true;
    b.bus = std::make_unique<ModbusRTU>(b.config.device, b.config.baud_rate);
    b.bus->set_timeout(config_.response_timeout_ms > 0 ? config_.response_timeout_ms : 500);
    if (!b.bus->open()) {
        const std::string err = "port " + b.config.device + ": " + b.bus->get_last_error();
        registry_.mark_branch_offline(b.config.id, err, now_ms());
        b.bus.reset();
        return false;
    }
    LOG_INFO("PeriodicPoller: branch " + std::to_string(b.config.id) + " opened " + b.config.device + " @"
             + std::to_string(b.config.baud_rate));
    return true;
}

PollResult PeriodicPoller::poll_board(Branch& b, const BoardConfig& bc) {
    BoardClient client(*b.bus, bc.address);
    PollResult r;
    const int retries = config_.max_retries;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        BoardData d;
        // ID термометров — каждый цикл: по ним видно, подключены ли счётчики
        if (client.read_status(d) && client.read_settings(d) && client.read_dac_readback(d) && client.read_ids(d)) {
            r.ok = true;
            r.data = d;
            break;
        }
        r.error = client.last_error();
        if (attempt < retries) std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
    }
    r.time_ms = now_ms();
    registry_.update(b.config.id, bc.address, r);
    return r;
}

void PeriodicPoller::execute(Branch& b, BoardCommand& cmd) {
    const BoardConfig* bc = nullptr;
    for (const auto& x : b.config.boards)
        if (x.address == cmd.address) bc = &x;
    bool ok = false;
    std::string report;
    if (!bc) {
        report = "board " + std::to_string(cmd.address) + " is not on branch " + std::to_string(b.config.id);
    } else if (!bc->enabled) {
        report = "board " + bc->name + " is disabled in the configuration";
    } else if (!ensure_bus(b)) {
        report = "port " + b.config.device + " is not available";
    } else {
        BoardClient client(*b.bus, cmd.address);
        switch (cmd.type) {
            case BoardCommand::Type::WRITE_SETPOINTS: {
                // Запись holding идемпотентна — можно повторять
                const auto& s = cmd.setpoints;
                for (int attempt = 0; attempt <= config_.max_retries && !ok; ++attempt) {
                    if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
                    bool w = true;
                    if (cmd.fields & BoardSetpoints::DAC) w = w && client.write_all_dac(s.dac);
                    if (cmd.fields & BoardSetpoints::HV_ENABLE) w = w && client.write_hv_enable(s.hv_enable);
                    if (cmd.fields & BoardSetpoints::GAIN_TRIP) w = w && client.write_gain_trip_enable(s.gain_trip_enable);
                    if (cmd.fields & BoardSetpoints::CYCLE_DELAY) w = w && client.write_cycle_delay(s.cycle_delay);
                    BoardData d;
                    if (!w || !client.read_settings(d)) {
                        report = client.last_error();
                        continue;
                    }
                    // Проверка: holding-регистры платы равны записанному
                    const auto now = BoardSetpoints::from_board(d);
                    const uint8_t bad = now.differs(s) & cmd.fields;
                    if (bad) {
                        report = "written, but board holds other values: " + describe_diff(s, now);
                        continue;
                    }
                    ok = true;
                    report = "written and read back: " + field_names(cmd.fields);
                }
                break;
            }
            case BoardCommand::Type::SAVE_TO_FLASH:
                ok = client.save_to_scratchpad(report);  // 103 -> 105 -> проверка scratchpad
                if (!ok) report = client.last_error();
                break;
            case BoardCommand::Type::LOAD_FROM_FLASH:
                ok = client.load_from_scratchpad();
                report = ok ? "loaded from scratchpad (101)" : client.last_error();
                break;
        }
        // Плата применяет уставки (и обновляет обратное чтение DAC) в своём цикле — дождаться
        // его, иначе первый опрос после записи покажет «DAC не применён»
        client.wait_next_cycle(5.0);
        poll_board(b, *bc);  // сразу показать результат в реестре
    }
    const std::string who = bc ? bc->name : "addr " + std::to_string(cmd.address);
    if (ok) LOG_INFO("Board " + who + ": " + to_string(cmd.type) + ": " + report);
    else LOG_WARNING("Board " + who + ": " + to_string(cmd.type) + " FAILED: " + report);
    if (cmd.done) cmd.done(ok, report);
}

void PeriodicPoller::run_branch(Branch& b) {
    const uint8_t id = b.config.id;
    uint64_t seen_wake = 0;
    bool poll_due = true;
    auto next_poll = std::chrono::steady_clock::now();

    while (true) {
        // ---- плановый цикл опроса (или poll_now)
        if (poll_due) {
            const auto cycle_start = std::chrono::steady_clock::now();
            if (ensure_bus(b)) {
                size_t polled = 0, failed = 0;
                for (const auto& bc : b.config.boards) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (stop_) break;
                    }
                    if (!bc.enabled) continue;
                    const PollResult r = poll_board(b, bc);
                    polled++;
                    failed += r.ok ? 0 : 1;
                }
                const auto st = b.bus->get_statistics();
                {
                    std::lock_guard<std::mutex> lock(b.stats_mutex);
                    b.stats.total_transactions = st.total_requests;
                    b.stats.successful_reads = st.successful_responses;
                    b.stats.failed_reads = st.total_requests - st.successful_responses;
                    b.stats.timeouts = st.timeouts;
                    b.stats.crc_errors = st.crc_errors;
                    b.stats.avg_response_time_ms = st.avg_response_time_ms;
                }
                // Ни одна плата не ответила — возможно, пропал сам порт (адаптер вынули):
                // в следующем цикле открыть его заново
                if (polled > 0 && failed == polled) {
                    LOG_WARNING("PeriodicPoller: branch " + std::to_string(id) + ": no board answered, reopening "
                                + b.config.device + " next cycle");
                    b.bus->close();
                    b.bus.reset();
                }
            }
            {
                std::lock_guard<std::mutex> lock(b.stats_mutex);
                b.cycles++;
            }
            if (callback_) callback_(id);
            next_poll = cycle_start + std::chrono::milliseconds(static_cast<int64_t>(interval_s_.load() * 1000));
            poll_due = false;
        }

        // ---- команды платам: сразу, по одной
        while (true) {
            BoardCommand cmd;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_ || b.queue.empty()) break;
                cmd = std::move(b.queue.front());
                b.queue.pop_front();
            }
            execute(b, cmd);
            if (callback_) callback_(id);
        }

        // ---- ждать: следующего цикла, poll_now, команды или stop
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, next_poll, [&] { return stop_ || wake_seq_ != seen_wake || !b.queue.empty(); });
        if (stop_) break;
        if (wake_seq_ != seen_wake) {
            seen_wake = wake_seq_;
            poll_due = true;
        }
        if (std::chrono::steady_clock::now() >= next_poll) poll_due = true;
    }
    if (b.bus) b.bus->close();
}

} // namespace danss
