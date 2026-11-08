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

struct PeriodicPoller::Branch {
    BranchConfig config;
    std::thread thread;
    std::unique_ptr<ModbusRTU> bus;
    mutable std::mutex stats_mutex;
    BranchStats stats{};
    uint64_t cycles = 0;
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
}

void PeriodicPoller::poll_now() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_seq_++;
    }
    cv_.notify_all();
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

void PeriodicPoller::run_branch(Branch& b) {
    const uint8_t id = b.config.id;
    const int retries = config_.max_retries;
    const auto retry_delay = std::chrono::milliseconds(config_.retry_delay_ms);
    uint64_t seen_wake = 0;

    while (true) {
        const auto cycle_start = std::chrono::steady_clock::now();

        // ---- порт ветки
        if (!b.bus || !b.bus->is_open()) {
            b.bus = std::make_unique<ModbusRTU>(b.config.device, b.config.baud_rate);
            b.bus->set_timeout(config_.response_timeout_ms > 0 ? config_.response_timeout_ms : 500);
            if (!b.bus->open()) {
                const std::string err = "port " + b.config.device + ": " + b.bus->get_last_error();
                registry_.mark_branch_offline(id, err, now_ms());
                b.bus.reset();
            } else {
                LOG_INFO("PeriodicPoller: branch " + std::to_string(id) + " opened " + b.config.device + " @"
                         + std::to_string(b.config.baud_rate));
            }
        }

        // ---- платы ветки
        if (b.bus) {
            size_t polled = 0, failed = 0;
            for (const auto& bc : b.config.boards) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stop_) break;
                }
                if (!bc.enabled) continue;
                BoardClient client(*b.bus, bc.address);
                PollResult r;
                for (int attempt = 0; attempt <= retries; ++attempt) {
                    BoardData d;
                    if (client.read_status(d) && client.read_settings(d) && client.read_dac_readback(d)) {
                        r.ok = true;
                        r.data = d;
                        break;
                    }
                    r.error = client.last_error();
                    if (attempt < retries) std::this_thread::sleep_for(retry_delay);
                }
                r.time_ms = now_ms();
                registry_.update(id, bc.address, r);
                polled++;
                failed += r.ok ? 0 : 1;
            }
            const auto st = b.bus->get_statistics();
            std::lock_guard<std::mutex> lock(b.stats_mutex);
            b.stats.total_transactions = st.total_requests;
            b.stats.successful_reads = st.successful_responses;
            b.stats.failed_reads = st.total_requests - st.successful_responses;
            b.stats.timeouts = st.timeouts;
            b.stats.crc_errors = st.crc_errors;
            b.stats.avg_response_time_ms = st.avg_response_time_ms;
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

        // ---- ждать следующего цикла (или stop / poll_now)
        std::unique_lock<std::mutex> lock(mutex_);
        const auto next = cycle_start + std::chrono::milliseconds(static_cast<int64_t>(interval_s_.load() * 1000));
        cv_.wait_until(lock, next, [&] { return stop_ || wake_seq_ != seen_wake; });
        seen_wake = wake_seq_;
        if (stop_) break;
    }
    if (b.bus) b.bus->close();
}

} // namespace danss
