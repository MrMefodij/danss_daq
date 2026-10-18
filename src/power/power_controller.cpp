#include "power/power_controller.h"
#include "common/constants.h"
#include "common/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace danss {

PowerSupplyController::PowerSupplyController(const PSUConfig& config)
    : config_(config)
    , psu_(std::make_unique<OwonPSU>(config.host, config.port))
{
    psu_->set_timeout(config_.response_timeout_ms > 0
        ? config_.response_timeout_ms
        : constants::DEFAULT_PSU_RESPONSE_TIMEOUT_MS);
}

PowerSupplyController::~PowerSupplyController() {
    // Do NOT ramp down on destruction — an unexpected process exit must not
    // silently start toggling SiPM HV. Explicit emergency_shutdown() (called
    // from the STOPPED transition / fault handling) is the intended path.
    disconnect();
}

bool PowerSupplyController::connect() {
    if (!psu_->connect()) {
        set_error("connect(): " + psu_->get_last_error());
        return false;
    }
    return true;
}

void PowerSupplyController::disconnect() {
    if (psu_) {
        psu_->disconnect();
    }
}

bool PowerSupplyController::is_connected() const {
    return psu_ && psu_->is_connected();
}

bool PowerSupplyController::apply_protection_limits() {
    std::lock_guard<std::mutex> lock(mutex_);

    bool all_ok = true;
    for (const auto& ch : config_.channels) {
        if (ch.role == PSUChannelRole::UNUSED) {
            continue;
        }
        if (!psu_->select_channel(ch.channel)) {
            set_error("apply_protection_limits(): select_channel(" + std::to_string(ch.channel)
                      + ") failed: " + psu_->get_last_error());
            all_ok = false;
            continue;
        }
        if (!psu_->set_current(ch.current_limit_a)) {
            set_error("apply_protection_limits(): set_current ch" + std::to_string(ch.channel)
                      + ": " + psu_->get_last_error());
            all_ok = false;
        }
        if (!psu_->set_voltage_protection(ch.ovp_limit_v)) {
            set_error("apply_protection_limits(): set_voltage_protection ch" + std::to_string(ch.channel)
                      + ": " + psu_->get_last_error());
            all_ok = false;
        }
        if (!psu_->set_current_protection(ch.ocp_limit_a)) {
            set_error("apply_protection_limits(): set_current_protection ch" + std::to_string(ch.channel)
                      + ": " + psu_->get_last_error());
            all_ok = false;
        }
        LOG_INFO("PowerSupplyController: ch" + std::to_string(ch.channel) + " (" + ch.name
                 + ") protection set: OVP=" + std::to_string(ch.ovp_limit_v)
                 + "V OCP=" + std::to_string(ch.ocp_limit_a) + "A");
    }
    return all_ok;
}

void PowerSupplyController::set_interlock_ready(bool ready) {
    bool was_ready = interlock_ready_.exchange(ready);
    if (was_ready && !ready) {
        LOG_WARNING("PowerSupplyController: interlock dropped — disabling interlocked channels");
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& ch : config_.channels) {
            if (ch.require_interlock) {
                if (psu_->select_channel(ch.channel)) {
                    if (ch.role == PSUChannelRole::SIPM_HV) {
                        double measured = 0.0;
                        double from_v = psu_->measure_voltage(measured) ? measured : ch.target_voltage_v;
                        ramp_voltage(ch.channel, from_v, 0.0, ch.ramp_step_v, ch.ramp_interval_ms);
                    }
                    psu_->set_output(false);
                }
            }
        }
    }
}

bool PowerSupplyController::is_interlock_ready() const {
    return interlock_ready_.load();
}

const PSUChannelConfig* PowerSupplyController::find_channel(uint8_t channel) const {
    for (const auto& ch : config_.channels) {
        if (ch.channel == channel) {
            return &ch;
        }
    }
    return nullptr;
}

bool PowerSupplyController::ramp_voltage(uint8_t channel, double from_v, double to_v,
                                          double step_v, uint32_t interval_ms) {
    (void)channel; // channel is assumed already selected by the caller
    if (step_v <= 0.0) {
        step_v = constants::DEFAULT_PSU_RAMP_STEP_V;
    }
    double current = from_v;
    double direction = (to_v >= current) ? 1.0 : -1.0;

    while (std::fabs(to_v - current) > 1e-6) {
        double next = current + direction * std::min(step_v, std::fabs(to_v - current));
        if (!psu_->set_voltage(next)) {
            set_error("ramp_voltage(): set_voltage failed at " + std::to_string(next)
                      + "V: " + psu_->get_last_error());
            return false; // left at 'current' (last successfully applied value)
        }
        current = next;
        if (std::fabs(to_v - current) > 1e-6) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }
    return true;
}

bool PowerSupplyController::enable_channel(uint8_t channel) {
    std::lock_guard<std::mutex> lock(mutex_);

    const PSUChannelConfig* cfg = find_channel(channel);
    if (!cfg) {
        set_error("enable_channel(): channel " + std::to_string(channel) + " not configured");
        return false;
    }
    if (cfg->require_interlock && !interlock_ready_.load()) {
        set_error("enable_channel(): channel " + std::to_string(channel)
                  + " (" + cfg->name + ") refused — interlock not ready");
        LOG_WARNING("PowerSupplyController: " + last_error_);
        return false;
    }
    if (!psu_->select_channel(channel)) {
        set_error("enable_channel(): select_channel failed: " + psu_->get_last_error());
        return false;
    }

    if (cfg->role == PSUChannelRole::SIPM_HV) {
        // Safe start: force 0V before enabling the output, then ramp up.
        if (!psu_->set_voltage(0.0)) {
            set_error("enable_channel(): could not zero ch" + std::to_string(channel)
                      + " before enabling: " + psu_->get_last_error());
            return false;
        }
        if (!psu_->set_output(true)) {
            set_error("enable_channel(): set_output(true) failed: " + psu_->get_last_error());
            return false;
        }
        LOG_INFO("PowerSupplyController: ramping ch" + std::to_string(channel) + " (" + cfg->name
                 + ") 0V -> " + std::to_string(cfg->target_voltage_v) + "V");
        if (!ramp_voltage(channel, 0.0, cfg->target_voltage_v, cfg->ramp_step_v, cfg->ramp_interval_ms)) {
            LOG_ERROR("PowerSupplyController: ramp-up aborted for ch" + std::to_string(channel)
                      + " — output left ON at last safe setpoint, not at target");
            return false;
        }
        return true;
    }

    // CPU_POWER / generic: set directly, no ramp needed for a board-electronics rail.
    if (!psu_->set_voltage(cfg->target_voltage_v)) {
        set_error("enable_channel(): set_voltage failed: " + psu_->get_last_error());
        return false;
    }
    if (!psu_->set_output(true)) {
        set_error("enable_channel(): set_output(true) failed: " + psu_->get_last_error());
        return false;
    }
    return true;
}

bool PowerSupplyController::disable_channel(uint8_t channel) {
    std::lock_guard<std::mutex> lock(mutex_);

    const PSUChannelConfig* cfg = find_channel(channel);
    if (!cfg) {
        set_error("disable_channel(): channel " + std::to_string(channel) + " not configured");
        return false;
    }
    if (!psu_->select_channel(channel)) {
        set_error("disable_channel(): select_channel failed: " + psu_->get_last_error());
        return false;
    }

    if (cfg->role == PSUChannelRole::SIPM_HV) {
        double measured = 0.0;
        double from_v = psu_->measure_voltage(measured) ? measured : cfg->target_voltage_v;
        if (!ramp_voltage(channel, from_v, 0.0, cfg->ramp_step_v, cfg->ramp_interval_ms)) {
            LOG_WARNING("PowerSupplyController: ramp-down for ch" + std::to_string(channel)
                        + " did not complete cleanly — forcing output off anyway");
        }
    }

    if (!psu_->set_output(false)) {
        set_error("disable_channel(): set_output(false) failed: " + psu_->get_last_error());
        return false;
    }
    return true;
}

void PowerSupplyController::emergency_shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_WARNING("PowerSupplyController: emergency_shutdown() — disabling all channels immediately, no ramp");
    for (const auto& ch : config_.channels) {
        if (ch.role == PSUChannelRole::UNUSED) {
            continue;
        }
        if (psu_->select_channel(ch.channel)) {
            if (!psu_->set_output(false)) {
                LOG_ERROR("PowerSupplyController: emergency_shutdown() failed to disable ch"
                          + std::to_string(ch.channel) + ": " + psu_->get_last_error());
            }
        } else {
            LOG_ERROR("PowerSupplyController: emergency_shutdown() could not select ch"
                      + std::to_string(ch.channel) + ": " + psu_->get_last_error());
        }
    }
}

std::vector<PSUChannelStatus> PowerSupplyController::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PSUChannelStatus> result;
    result.reserve(config_.channels.size());

    uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    for (const auto& ch : config_.channels) {
        PSUChannelStatus status{};
        status.channel = ch.channel;
        status.timestamp_ms = now_ms;

        if (ch.role == PSUChannelRole::UNUSED) {
            status.data_valid = false;
            status.error_message = "channel not in use";
            result.push_back(status);
            continue;
        }

        if (!psu_->select_channel(ch.channel)) {
            status.data_valid = false;
            status.error_message = psu_->get_last_error();
            result.push_back(status);
            continue;
        }

        double v = 0.0, i = 0.0, p = 0.0;
        bool ok = psu_->measure_voltage(v) && psu_->measure_current(i) && psu_->measure_power(p);
        if (!ok) {
            status.data_valid = false;
            status.error_message = psu_->get_last_error();
            result.push_back(status);
            continue;
        }

        status.measured_voltage_v = v;
        status.measured_current_a = i;
        status.measured_power_w = p;
        status.data_valid = true;

        // Output state as read back from the instrument for this channel;
        // OUTPut:STATe:ALL? avoids a 4th round-trip per channel.
        std::array<bool, 3> states{};
        if (psu_->get_all_output_states(states) && ch.channel >= 1 && ch.channel <= 3) {
            status.output_enabled = states[ch.channel - 1];
        }

        result.push_back(status);
    }

    return result;
}

void PowerSupplyController::set_error(const std::string& error) {
    last_error_ = error;
}

} // namespace danss
