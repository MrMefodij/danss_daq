#ifndef DANSS_POWER_CONTROLLER_H
#define DANSS_POWER_CONTROLLER_H

#include "common/types.h"
#include "power/owon_psu.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace danss {

/// @brief High-level control of the OWON ODP6033 power supply for Slow Control.
///
/// Wraps the low-level SCPI client (OwonPSU) with the policy this project
/// needs on top of it:
///   - applies the configured OVP/OCP protection limits to every channel
///     once at startup (not relying on whatever was left on the instrument
///     front panel from a previous session);
///   - gates output enable for any channel marked `require_interlock` (in
///     practice the SiPM HV channels) behind an external readiness flag —
///     e.g. "all SuperFGD boards are online" as reported by SlowControl;
///   - ramps HV channels up/down in configurable steps instead of slamming
///     the setpoint, since a bare `VOLTage 55` on a channel that was at 0V
///     is an instant ~55V step across the SiPM bias line;
///   - offers one-call emergency_shutdown() for the ANY -> STOPPED
///     transition and for error handling.
///
/// This class does not talk to MIDAS directly — see power_equipment.cpp
/// (TODO, Phase 3) for the SlowControl-style Equipment wrapper that will
/// call into this from read_event()/begin_of_run()/end_of_run().
class PowerSupplyController {
public:
    explicit PowerSupplyController(const PSUConfig& config);
    ~PowerSupplyController();

    PowerSupplyController(const PowerSupplyController&) = delete;
    PowerSupplyController& operator=(const PowerSupplyController&) = delete;

    /// @brief Connect to the instrument.
    bool connect();
    void disconnect();
    bool is_connected() const;

    /// @brief Push the configured current limit and OVP/OCP protection
    /// thresholds to every channel. Call once right after connect(), before
    /// enabling any output.
    /// @return true if every channel was configured successfully
    bool apply_protection_limits();

    /// @brief Set the external interlock state (e.g. driven by SlowControl's
    /// board-online tracking). While false, enable_channel() refuses any
    /// channel configured with require_interlock == true.
    void set_interlock_ready(bool ready);
    bool is_interlock_ready() const;

    /// @brief Bring a channel up to its configured target_voltage_v and turn
    /// its output on.
    ///   - CPU_POWER channels: output enabled, voltage set directly to
    ///     target (no ramp needed for a fixed low-voltage board supply).
    ///   - SIPM_HV channels: voltage forced to 0V first, output enabled,
    ///     then ramped up in ramp_step_v increments every ramp_interval_ms
    ///     until target_voltage_v is reached (or an error occurs).
    /// Refused (returns false) if the channel requires the interlock and
    /// it is not currently ready.
    bool enable_channel(uint8_t channel);

    /// @brief Ramp a SIPM_HV channel back down to 0V in the same step size,
    /// then disable the output. CPU_POWER channels are disabled directly.
    bool disable_channel(uint8_t channel);

    /// @brief Disable every configured channel's output immediately —
    /// no ramp-down. For the ANY -> STOPPED transition and fault handling.
    void emergency_shutdown();

    /// @brief Read back measured voltage/current/power and output state for
    /// every configured channel. Safe to call on the SlowControl poll cycle.
    std::vector<PSUChannelStatus> poll();

    std::string get_last_error() const { return last_error_; }

private:
    const PSUChannelConfig* find_channel(uint8_t channel) const;

    /// @brief Reconnect (and take remote control) if the link was dropped.
    bool ensure_connected();

    /// @brief Drive the selected channel's voltage from `from_v` to `to_v`
    /// in `step_v` increments (sign inferred), sleeping `interval_ms`
    /// between steps. Aborts (returning false) on the first communication
    /// error, on emergency_shutdown(), or — when ramping up an interlocked
    /// channel — when the interlock drops, leaving the channel at its last
    /// successfully-set voltage — never leaves it silently jumping to the
    /// target on failure.
    bool ramp_voltage(uint8_t channel, double from_v, double to_v, double step_v, uint32_t interval_ms);

    void set_error(const std::string& error);

    PSUConfig config_;
    std::unique_ptr<OwonPSU> psu_;
    std::atomic<bool> interlock_ready_{false};
    std::atomic<bool> abort_ramp_{false};
    bool protections_ok_ = false;
    mutable std::mutex mutex_; // serializes channel operations (enable/disable/ramp/poll)
    std::string last_error_;
};

} // namespace danss

#endif // DANSS_POWER_CONTROLLER_H
