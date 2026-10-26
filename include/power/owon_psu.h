#ifndef DANSS_POWER_OWON_PSU_H
#define DANSS_POWER_OWON_PSU_H

#include <boost/asio.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace danss {

/// @brief Low-level SCPI-over-TCP client for the OWON ODP series power supply
///        (ODP3033 / ODP3063 / ODP6033 — this project targets the ODP6033,
///        3 channels: CH1/CH2 0-60V/3A, CH3 0-6V/3A).
///
/// The instrument accepts a raw ASCII SCPI command per line terminated with
/// '\n' on the configured TCP port (3000 by default) and answers queries
/// (commands ending in '?') with a single line, also '\n'-terminated.
///
/// Thread-safe for concurrent access (single mutex around a transaction,
/// mirroring ModbusRTU's model).
class OwonPSU {
public:
    /// @brief Constructor
    /// @param host PSU IP address or hostname (e.g. d2pwr1.danss.local)
    /// @param port SCPI TCP port (default: 3000)
    explicit OwonPSU(const std::string& host, uint16_t port = 3000);

    /// @brief Destructor - closes the TCP connection
    ~OwonPSU();

    // Disable copy and move
    OwonPSU(const OwonPSU&) = delete;
    OwonPSU& operator=(const OwonPSU&) = delete;
    OwonPSU(OwonPSU&&) = delete;
    OwonPSU& operator=(OwonPSU&&) = delete;

    /// @brief Open TCP connection to the instrument
    /// @return true on success
    bool connect();

    /// @brief Close TCP connection
    void disconnect();

    /// @brief Check if the socket is connected
    bool is_connected() const;

    /// @brief Set query response timeout (wait for the reply line)
    void set_response_timeout(uint32_t timeout_ms);

    /// @brief Set TCP connect timeout (default 2000 ms)
    void set_connect_timeout(uint32_t timeout_ms);

    // =========================================================================
    // Identification / mode
    // =========================================================================

    /// @brief *IDN? query
    bool get_idn(std::string& idn_out);

    /// @brief SYSTem:REMote / SYSTem:LOCal — take/release front-panel control.
    /// Not strictly required (the instrument accepts SCPI regardless), but
    /// recommended while Slow Control owns the channel configuration.
    bool set_remote(bool remote);

    // =========================================================================
    // Channel selection
    // =========================================================================

    /// @brief INSTrument:NSELect <1|2|3> — select the active channel that the
    /// single-channel setters/getters below operate on.
    /// @param channel Physical channel number (1-3)
    bool select_channel(uint8_t channel);

    // =========================================================================
    // Configuration (operate on the currently selected channel)
    // =========================================================================

    /// @brief [SOURce:]VOLTage <value> — set output voltage setpoint
    bool set_voltage(double volts);

    /// @brief [SOURce:]CURRent <value> — set output current limit
    bool set_current(double amps);

    /// @brief [SOURce:]VOLTage:LIMit <value> — programmed OVP threshold
    bool set_voltage_protection(double volts);

    /// @brief [SOURce:]CURRent:LIMit <value> — programmed OCP threshold
    bool set_current_protection(double amps);

    /// @brief OUTPut[:STATe] <0|1> — enable/disable the selected channel's output
    bool set_output(bool on);

    /// @brief [SOURce:]VOLTage:LIMit? / CURRent:LIMit? — read back OVP / OCP threshold
    bool get_voltage_protection(double& volts_out);
    bool get_current_protection(double& amps_out);

    // =========================================================================
    // Measurements (operate on the currently selected channel unless noted)
    // =========================================================================

    /// @brief MEASure:VOLTage:DC? — measured output voltage
    bool measure_voltage(double& volts_out);

    /// @brief MEASure:CURRent:DC? — measured output current
    bool measure_current(double& amps_out);

    /// @brief MEASure:POWer:DC? — measured output power
    bool measure_power(double& watts_out);

    /// @brief MEASure:VOLTage:ALL:DC? — measured voltage on all 3 channels
    bool measure_all_voltages(std::array<double, 3>& out);

    /// @brief MEASure:CURRent:ALL:DC? — measured current on all 3 channels
    bool measure_all_currents(std::array<double, 3>& out);

    /// @brief OUTPut:STATe:ALL? — output enabled state on all 3 channels
    bool get_all_output_states(std::array<bool, 3>& out);

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Statistics {
        uint64_t total_commands = 0;
        uint64_t successful_commands = 0;
        uint64_t timeouts = 0;
        uint64_t protocol_errors = 0;
        double avg_response_time_ms = 0.0;
    };

    Statistics get_statistics() const;
    void reset_statistics();

    /// @brief Get last error message
    std::string get_last_error() const;

private:
    // =========================================================================
    // Internal methods
    // =========================================================================

    /// @brief Send a SCPI command with no expected reply (e.g. VOLT 55).
    bool send_command(const std::string& cmd);

    /// @brief Send a SCPI query and read back a single '\n'-terminated reply
    /// (e.g. "MEAS:VOLT:DC?"). Uses a select()-based read loop with the
    /// configured timeout, the same pattern used by ModbusRTU::transact() —
    /// a blocking boost::asio::read() on a socket that never answers (PSU
    /// unreachable/powered off) must not hang the poller thread.
    bool send_query(const std::string& cmd, std::string& reply_out);

    /// @brief Low-level line write/read helpers
    bool write_line(const std::string& line);
    bool read_line(std::string& line_out);

    void set_error(const std::string& error);
    void update_stats(bool success, double response_time_ms);

    // =========================================================================
    // Member variables
    // =========================================================================

    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;

    std::string host_;
    uint16_t port_;
    uint32_t response_timeout_ms_;
    uint32_t connect_timeout_ms_;

    mutable std::mutex mutex_; // Thread safety
    mutable std::mutex error_mutex_;
    std::string last_error_;

    Statistics stats_;
};

} // namespace danss

#endif // DANSS_POWER_OWON_PSU_H
