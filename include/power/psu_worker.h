#ifndef DANSS_PSU_WORKER_H
#define DANSS_PSU_WORKER_H

#include "common/types.h"
#include "power/power_controller.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace danss {

/// @brief Снимок состояния блока питания для MIDAS (и любого другого потребителя).
struct PsuSnapshot {
    uint64_t seq = 0;                          ///< растёт с каждым опросом
    bool connected = false;
    bool protections_ok = false;               ///< OVP/OCP записаны и подтверждены
    bool interlock_ready = false;
    std::string busy;                          ///< "" или что делается сейчас ("ramp ch1 -> 55 V")
    std::string last_error;                    ///< последняя ошибка ("" — нет)
    uint64_t error_seq = 0;                    ///< растёт с каждой новой ошибкой
    std::array<bool, 3> desired_on{};          ///< что приказано (индекс = канал - 1)
    std::vector<PSUChannelStatus> channels;    ///< последний опрос
    PSUConfig config;                          ///< действующая конфигурация
};

/// @brief Поток управления блоком питания поверх PowerSupplyController.
///
/// Зачем: разгон HV до 55 В длится ~1 минуту. Если делать его в обработчике MIDAS,
/// frontend на минуту перестанет отвечать (переходы ранов, RPC, watchdog).
/// Здесь все долгие операции идут в своём потоке, а команды не блокируют вызывающего:
///  - request_enable() задаёт ЖЕЛАЕМОЕ состояние канала; если приходит «выключить»
///    посреди разгона — разгон прерывается и канал плавно опускается;
///  - set_interlock(false) и emergency_shutdown() действуют немедленно, в отдельном потоке,
///    даже если рабочий поток занят разгоном;
///  - request_config() — новая (уже проверенная ConfigLoader) конфигурация: пороги
///    записываются заново, включённые каналы переходят на новую цель.
/// Если команда не выполнилась (нет интерлока, ошибка связи), желаемое состояние
/// канала сбрасывается в «выключен», а причина — в snapshot().last_error.
class PsuWorker {
public:
    explicit PsuWorker(const PSUConfig& config);
    ~PsuWorker();

    PsuWorker(const PsuWorker&) = delete;
    PsuWorker& operator=(const PsuWorker&) = delete;

    /// @brief Подключиться, записать пороги, прочитать текущие выходы и запустить поток.
    /// Каналы, уже включённые на приборе, считаются «желаемо включёнными» — при перезапуске
    /// frontend ничего не выключает и не включает сам.
    void start();
    void stop();

    void request_enable(uint8_t channel, bool on);
    void request_config(const PSUConfig& config);
    void request_poll();
    void set_interlock(bool ready);
    void emergency_shutdown();

    PsuSnapshot snapshot() const;

private:
    void run();
    void do_poll();
    void apply_pending_config();
    void apply_channel(uint8_t channel, bool on);
    void record_error(const std::string& msg);
    void run_safety(std::function<void()> fn);

    std::shared_ptr<PowerSupplyController> ctrl_;

    mutable std::mutex mutex_;          // защищает всё ниже
    std::condition_variable cv_;
    std::array<bool, 3> desired_{};     // приказано
    std::array<bool, 3> applied_{};     // на выходе прибора (по последнему опросу)
    std::array<bool, 3> retry_blocked_{};  // команда не удалась — повтор не раньше следующего опроса
    std::optional<PSUConfig> pending_config_;
    bool poll_requested_ = true;
    bool stop_ = false;
    bool interlock_ = false;
    std::string busy_;
    uint8_t busy_channel_ = 0;
    bool busy_on_ = false;
    PsuSnapshot snap_;

    std::thread thread_;

    // Потоки безопасности (снятие интерлока / аварийное выключение) не ждут рабочий поток
    std::mutex safety_mutex_;
    std::condition_variable safety_cv_;
    int safety_active_ = 0;
};

} // namespace danss

#endif // DANSS_PSU_WORKER_H
