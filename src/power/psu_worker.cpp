#include "power/psu_worker.h"
#include "common/logger.h"

#include <chrono>

namespace danss {

namespace {
std::string ch_name(uint8_t ch) { return "ch" + std::to_string(ch); }

const PSUChannelConfig* find(const PSUConfig& cfg, uint8_t ch) {
    for (const auto& c : cfg.channels)
        if (c.channel == ch) return &c;
    return nullptr;
}
} // namespace

PsuWorker::PsuWorker(const PSUConfig& config)
    : ctrl_(std::make_shared<PowerSupplyController>(config)) {
    snap_.config = config;
}

PsuWorker::~PsuWorker() {
    stop();
}

// =============================================================================
// Запуск / остановка
// =============================================================================

void PsuWorker::start() {
    auto ctrl = ctrl_;
    if (!ctrl->connect()) {
        record_error("connect: " + ctrl->get_last_error());
    } else if (!ctrl->apply_protection_limits()) {
        record_error("apply_protection_limits: " + ctrl->get_last_error());
    }
    do_poll();
    {
        // Перезапуск frontend не должен ничего переключать: что включено на приборе,
        // то и считаем приказанным.
        std::lock_guard<std::mutex> lock(mutex_);
        desired_ = applied_;
    }
    thread_ = std::thread(&PsuWorker::run, this);
}

void PsuWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    ctrl_->abort_ramp();  // не ждать окончания минутного разгона
    if (thread_.joinable()) thread_.join();
    // Дождаться потоков безопасности (снятие интерлока / аварийное выключение)
    std::unique_lock<std::mutex> lock(safety_mutex_);
    safety_cv_.wait(lock, [this] { return safety_active_ == 0; });
}

// =============================================================================
// Команды (не блокируют вызывающего)
// =============================================================================

void PsuWorker::request_enable(uint8_t channel, bool on) {
    if (channel < 1 || channel > 3) return;
    std::shared_ptr<PowerSupplyController> ctrl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired_[channel - 1] = on;
        retry_blocked_[channel - 1] = false;
        // «Выключить» посреди разгона этого канала — прервать разгон, дальше рабочий поток
        // плавно опустит канал. Спуск не прерываем: disable_channel() при сбое спуска
        // выключил бы выход скачком.
        if (!on && busy_channel_ == channel && busy_on_) ctrl = ctrl_;
    }
    if (ctrl) ctrl->abort_ramp();
    cv_.notify_all();
}

void PsuWorker::request_config(const PSUConfig& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_config_ = config;
    }
    cv_.notify_all();
}

void PsuWorker::request_poll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        poll_requested_ = true;
    }
    cv_.notify_all();
}

void PsuWorker::set_interlock(bool ready) {
    std::shared_ptr<PowerSupplyController> ctrl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (interlock_ == ready) return;
        interlock_ = ready;
        snap_.interlock_ready = ready;
        ctrl = ctrl_;
        if (!ready) {
            // Интерлок пропал: каналы с интерлоком больше не должны быть включены
            for (const auto& c : snap_.config.channels)
                if (c.require_interlock && c.channel >= 1 && c.channel <= 3) desired_[c.channel - 1] = false;
        }
    }
    if (ready) {
        ctrl->set_interlock_ready(true);  // быстро, без блокировок
        LOG_INFO("PsuWorker: interlock ready");
    } else {
        LOG_WARNING("PsuWorker: interlock dropped — ramping down interlocked channels");
        // set_interlock_ready(false) прерывает разгон и опускает HV — это может длиться минуту,
        // поэтому в отдельном потоке.
        run_safety([ctrl] { ctrl->set_interlock_ready(false); });
    }
    cv_.notify_all();
}

void PsuWorker::emergency_shutdown() {
    std::shared_ptr<PowerSupplyController> ctrl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired_.fill(false);
        ctrl = ctrl_;
    }
    LOG_WARNING("PsuWorker: emergency shutdown requested");
    run_safety([ctrl] { ctrl->emergency_shutdown(); });
    cv_.notify_all();
}

void PsuWorker::run_safety(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(safety_mutex_);
        ++safety_active_;
    }
    std::thread([this, fn] {
        fn();
        request_poll();
        std::lock_guard<std::mutex> lock(safety_mutex_);
        --safety_active_;
        safety_cv_.notify_all();
    }).detach();
}

PsuSnapshot PsuWorker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PsuSnapshot s = snap_;
    s.desired_on = desired_;
    s.busy = busy_;
    s.interlock_ready = interlock_;
    return s;
}

// =============================================================================
// Рабочий поток
// =============================================================================

void PsuWorker::run() {
    using clock = std::chrono::steady_clock;
    auto next_poll = clock::now();

    while (true) {
        uint8_t todo_ch = 0;
        bool todo_on = false, todo_config = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto pending = [this] {
                if (stop_ || poll_requested_ || pending_config_) return true;
                for (size_t i = 0; i < 3; ++i)
                    if (desired_[i] != applied_[i] && !retry_blocked_[i]) return true;
                return false;
            };
            cv_.wait_until(lock, next_poll, pending);
            if (stop_) return;
            if (pending_config_) {
                todo_config = true;
            } else {
                for (uint8_t ch = 1; ch <= 3; ++ch) {
                    if (desired_[ch - 1] != applied_[ch - 1] && !retry_blocked_[ch - 1]) {
                        todo_ch = ch;
                        todo_on = desired_[ch - 1];
                        break;
                    }
                }
            }
        }

        if (todo_config) {
            apply_pending_config();
            do_poll();
        } else if (todo_ch) {
            apply_channel(todo_ch, todo_on);
            do_poll();  // сверить applied_ с тем, что на самом деле на выходе
        } else {
            do_poll();
        }

        uint32_t period = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            period = snap_.config.poll_interval_sec > 0 ? snap_.config.poll_interval_sec : 10;
            retry_blocked_.fill(false);  // неудачные команды — не чаще одного раза за опрос
            if (todo_ch && desired_[todo_ch - 1] != applied_[todo_ch - 1] && desired_[todo_ch - 1] == todo_on)
                retry_blocked_[todo_ch - 1] = true;
        }
        next_poll = clock::now() + std::chrono::seconds(period);
    }
}

void PsuWorker::apply_channel(uint8_t ch, bool on) {
    std::shared_ptr<PowerSupplyController> ctrl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ctrl = ctrl_;
        busy_channel_ = ch;
        busy_on_ = on;
        const PSUChannelConfig* c = find(snap_.config, ch);
        busy_ = (on ? "enable " : "disable ") + ch_name(ch)
                + (c ? " (" + c->name + (on ? ", -> " + std::to_string(c->target_voltage_v).substr(0, 5) + " V)" : ")") : "");
    }
    LOG_INFO("PsuWorker: " + std::string(on ? "enable " : "disable ") + ch_name(ch));
    const bool ok = on ? ctrl->enable_channel(ch) : ctrl->disable_channel(ch);
    std::string err = ok ? "" : ctrl->get_last_error();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        busy_.clear();
        busy_channel_ = 0;
        // Не вышло включить (нет интерлока, электроника выключена, ...) — команда отменяется,
        // иначе поток пытался бы снова и снова.
        if (!ok && on) desired_[ch - 1] = false;
    }
    if (!ok) record_error((on ? "enable " : "disable ") + ch_name(ch) + ": " + err);
}

void PsuWorker::apply_pending_config() {
    PSUConfig cfg;
    std::shared_ptr<PowerSupplyController> ctrl;
    std::array<bool, 3> applied{}, desired{};
    PSUConfig old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = *pending_config_;
        pending_config_.reset();
        ctrl = ctrl_;
        applied = applied_;
        desired = desired_;
        old = snap_.config;
    }

    const bool any_on = applied[0] || applied[1] || applied[2];
    if (cfg.host != old.host || cfg.port != old.port) {
        if (any_on) {
            record_error("config: смена host/port отклонена — сначала выключите все выходы");
            return;
        }
        auto fresh = std::make_shared<PowerSupplyController>(cfg);
        bool interlock = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interlock = interlock_;
        }
        fresh->set_interlock_ready(interlock);
        if (!fresh->connect()) record_error("connect " + cfg.host + ": " + fresh->get_last_error());
        else if (!fresh->apply_protection_limits()) record_error("apply_protection_limits: " + fresh->get_last_error());
        std::lock_guard<std::mutex> lock(mutex_);
        ctrl_ = fresh;
        snap_.config = cfg;
        LOG_INFO("PsuWorker: new instrument " + cfg.host + ":" + std::to_string(cfg.port));
        return;
    }

    // Канал включён, а в новой конфигурации у него другая роль — сначала выключить по старой
    for (uint8_t ch = 1; ch <= 3; ++ch) {
        const PSUChannelConfig* o = find(old, ch);
        const PSUChannelConfig* n = find(cfg, ch);
        if (applied[ch - 1] && o && (!n || n->role != o->role)) {
            apply_channel(ch, false);
            std::lock_guard<std::mutex> lock(mutex_);
            desired_[ch - 1] = false;
            desired[ch - 1] = false;
        }
    }

    if (!ctrl->update_config(cfg)) {
        record_error("config: " + ctrl->get_last_error());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.config = cfg;
    }
    LOG_INFO("PsuWorker: configuration updated — writing protection limits");
    if (!ctrl->apply_protection_limits()) {
        record_error("apply_protection_limits: " + ctrl->get_last_error());
        return;
    }
    // Включённые каналы с новой целью — плавно перевести на неё
    for (uint8_t ch = 1; ch <= 3; ++ch) {
        const PSUChannelConfig* o = find(old, ch);
        const PSUChannelConfig* n = find(cfg, ch);
        if (applied[ch - 1] && desired[ch - 1] && o && n && n->role != PSUChannelRole::UNUSED
            && n->target_voltage_v != o->target_voltage_v) {
            apply_channel(ch, true);
        }
    }
}

void PsuWorker::do_poll() {
    std::shared_ptr<PowerSupplyController> ctrl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ctrl = ctrl_;
        poll_requested_ = false;
    }
    auto statuses = ctrl->poll();
    // Связь была потеряна и восстановлена, а пороги не подтвердились — записать заново
    if (ctrl->is_connected() && !ctrl->protections_ok()) {
        if (!ctrl->apply_protection_limits()) record_error("apply_protection_limits: " + ctrl->get_last_error());
    }
    bool any_valid = false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : statuses) {
        if (!s.data_valid) continue;
        any_valid = true;
        if (s.channel >= 1 && s.channel <= 3) applied_[s.channel - 1] = s.output_enabled;
    }
    snap_.channels = statuses;
    snap_.connected = ctrl->is_connected() && any_valid;
    snap_.protections_ok = ctrl->protections_ok();
    snap_.seq++;
}

void PsuWorker::record_error(const std::string& msg) {
    LOG_ERROR("PsuWorker: " + msg);
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.last_error = msg;
    snap_.error_seq++;
}

} // namespace danss
