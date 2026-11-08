#ifndef DANSS_PERIODIC_POLLER_H
#define DANSS_PERIODIC_POLLER_H

#include "common/types.h"
#include "rs485/board_registry.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace danss {

/// @brief Периодический опрос плат: свой поток на каждую ветку RS-485.
///
/// За цикл для каждой включённой платы ветки: input 0–27 (состояние), holding 0–100
/// (уставки), input 150–245 (обратное чтение DAC) — через BoardClient, с повторами
/// max_retries / retry_delay_ms из конфига. Результат — в BoardRegistry (там же
/// вычисляются ONLINE/STALE/OFFLINE и проверки). Ветки опрашиваются параллельно.
///
/// Порт ветки не открылся или пропал — все её платы OFFLINE, повтор в следующем цикле.
class PeriodicPoller {
public:
    /// Вызывается из потока ветки после каждого цикла ветки
    using CycleCallback = std::function<void(uint8_t branch_id)>;

    PeriodicPoller(const SystemConfig& config, BoardRegistry& registry);
    ~PeriodicPoller();

    PeriodicPoller(const PeriodicPoller&) = delete;
    PeriodicPoller& operator=(const PeriodicPoller&) = delete;

    void set_cycle_callback(CycleCallback cb) { callback_ = std::move(cb); }
    /// Переопределить период опроса (секунды), например для отладки
    void set_poll_interval(double seconds) { interval_s_ = seconds; }

    void start();
    void stop();             ///< возвращается быстро: текущий запрос дожидается своего таймаута
    void poll_now();         ///< начать следующий цикл на всех ветках сейчас

    /// Статистика Modbus по ветке
    BranchStats branch_stats(uint8_t branch_id) const;
    uint64_t cycles(uint8_t branch_id) const;

private:
    struct Branch;
    void run_branch(Branch& b);

    SystemConfig config_;
    BoardRegistry& registry_;
    CycleCallback callback_;
    std::atomic<double> interval_s_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    uint64_t wake_seq_ = 0;     // poll_now() увеличивает — ветки просыпаются

    std::vector<std::unique_ptr<Branch>> branches_;
};

} // namespace danss

#endif // DANSS_PERIODIC_POLLER_H
