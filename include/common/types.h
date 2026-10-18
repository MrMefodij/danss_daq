#ifndef DANSS_COMMON_TYPES_H
#define DANSS_COMMON_TYPES_H

#include <cstdint>
#include <string>
#include <vector>
#include <array>

#include "common/constants.h"

namespace danss {

// =============================================================================
// Board Data Structures
// =============================================================================

/// @brief SiPM channel data from a single board
struct SiPMData {
    uint16_t leakage_current = 0;  ///< ADC value (input 3-14)
    uint16_t temperature = 0;      ///< ADC value (input 15-26)
    std::string thermometer_id;    ///< 1-Wire ID from EEPROM (input 100-147, 4 registers each)
};

/// @brief Complete board data snapshot (register map v2, see doc/MODBUS_MAP_v2.md)
struct BoardData {
    // Board identification
    uint8_t modbus_address = 0;  ///< Modbus slave address (1-247)
    std::string board_name;      ///< Human-readable name (e.g., "PMT-01-01")

    // ---- Input registers (read-only) ----
    uint16_t update_counter = 0;   ///< Input 0: растёт в конце каждого цикла обновления
    uint16_t hv_status = 0;        ///< Input 1: 0ttt eeee eeee eeee (trip по аналоговым платам, HV по каналам)
    uint16_t gain_trip_status = 0; ///< Input 2: 0000 0rrr 0000 0aaa (разрешение trip, усиление)

    std::array<SiPMData, constants::NUM_SIPM_CHANNELS> sipm_channels{}; ///< 12 SiPM channels

    uint16_t cpu_temperature = 0;  ///< Input 27
    uint16_t cpu_temp_offset = 0;  ///< Input 246: смещение датчика температуры CPU
    std::string cpu_id;            ///< Input 148-149 (EEPROM)
    std::array<uint16_t, constants::NUM_DAC_CHANNELS> dac_readback{}; ///< Input 150-245: применённые DAC

    // ---- Holding registers (read-write) ----
    std::array<uint16_t, constants::NUM_DAC_CHANNELS> dac_settings{}; ///< Holding 0-95: уставки DAC
    uint16_t hv_enable = 0;        ///< Holding 96: Counter ON/OFF (SiPM HV enable), бит на канал
    uint16_t gain_trip_enable = 0; ///< Holding 97: усиление и разрешение trip, бит на аналоговую плату
    uint16_t global_csr = 0;       ///< Holding 98: статус последней команды (CSR_STATUS_*)
    uint16_t cycle_delay = 0;      ///< Holding 99: задержка между циклами
    uint16_t device_address = 0;   ///< Holding 100: адрес устройства

    // ---- Metadata ----
    uint64_t timestamp_ms = 0;     ///< Unix timestamp in milliseconds
    bool data_valid = false;       ///< True if data was successfully read
    bool data_fresh = false;       ///< True if update_counter changed since previous poll
    std::string error_message;     ///< Error description if data_valid == false

    // ---- Helpers ----
    bool hv_channel_enabled(size_t ch) const { return constants::hv_channel_enabled(hv_status, ch); }
    bool analog_board_tripped(size_t board) const { return constants::analog_board_tripped(hv_status, board); }
    bool any_trip() const { return constants::tripped_analog_boards(hv_status) != 0; }
    /// Каналы, у которых применённое значение DAC отличается от уставки
    std::vector<size_t> dac_mismatches() const {
        std::vector<size_t> out;
        for (size_t i = 0; i < dac_settings.size(); ++i)
            if (dac_settings[i] != dac_readback[i]) out.push_back(i);
        return out;
    }
};

/// @brief Image of the board scratchpad (non-volatile memory), input 250-761
struct ScratchpadImage {
    std::array<uint16_t, constants::SCRATCHPAD_SIZE_WORDS> words{};

    /// Сумма всех слов по модулю 2^16 (должна быть SCRATCHPAD_CHECKSUM_VALUE)
    uint16_t sum() const {
        uint16_t s = 0;
        for (uint16_t w : words) s = static_cast<uint16_t>(s + w);
        return s;
    }
    bool checksum_ok() const { return sum() == constants::SCRATCHPAD_CHECKSUM_VALUE; }
    uint16_t dac(size_t ch) const { return words[constants::SCRATCHPAD_DAC_START + ch]; }
    uint16_t hv_enable() const { return words[constants::SCRATCHPAD_HV_ENABLE]; }
    uint16_t cycle_delay() const { return words[constants::SCRATCHPAD_CYCLE_DELAY]; }
    uint16_t modbus_address() const { return words[constants::SCRATCHPAD_MODBUS_ADDRESS]; }
};

// =============================================================================
// Configuration Structures
// =============================================================================

/// @brief Single board configuration
struct BoardConfig {
    uint8_t address;            ///< Modbus address
    std::string name;           ///< Board identifier
    bool enabled;               ///< Enable/disable polling
    
    // EEPROM reference data (for validation)
    std::vector<std::string> thermometer_ids; ///< Expected thermometer IDs
    std::string cpu_id;         ///< Expected CPU ID
};

/// @brief RS-485 branch configuration
struct BranchConfig {
    uint8_t id;                 ///< Branch number (0-7)
    std::string device;         ///< Serial device path (e.g., /dev/ttyUSB0)
    uint32_t baud_rate;         ///< Communication speed (default: 19200)
    uint8_t data_bits;          ///< Data bits (default: 8)
    char parity;                ///< Parity: 'N', 'E', 'O' (default: 'N')
    uint8_t stop_bits;          ///< Stop bits (default: 1)
    
    std::vector<BoardConfig> boards; ///< Boards on this branch
};

// =============================================================================
// Power Supply (OWON ODP6033) Configuration
// =============================================================================

/// @brief Role assigned to a power-supply output channel
enum class PSUChannelRole : uint8_t {
    UNUSED = 0,
    CPU_POWER = 1,   ///< Fixed low-voltage supply for board electronics (~5.5-6V)
    SIPM_HV = 2      ///< High voltage bias for SiPM (~55V), ramped and interlocked
};

/// @brief Configuration for a single PSU output channel
struct PSUChannelConfig {
    uint8_t channel;             ///< Physical channel number (1-3)
    PSUChannelRole role;         ///< What this channel is used for
    std::string name;            ///< Human-readable label
    double target_voltage_v;     ///< Desired operating voltage
    double current_limit_a;      ///< Normal operating current limit (CURRent[:LEVel])
    double ovp_limit_v;          ///< Programmed overvoltage protection threshold
    double ocp_limit_a;          ///< Programmed overcurrent protection threshold
    double ramp_step_v;          ///< Voltage step per ramp iteration
    uint32_t ramp_interval_ms;   ///< Delay between ramp steps
    bool require_interlock;      ///< If true, output enable is gated by external interlock
};

/// @brief Configuration for the OWON ODP6033 power supply link
struct PSUConfig {
    std::string host;                       ///< PSU IP/hostname, e.g. d2pwr1.danss.local
    uint16_t port;                          ///< SCPI TCP port (default 3000)
    std::vector<PSUChannelConfig> channels; ///< Configured output channels
    uint32_t poll_interval_sec;             ///< Measurement poll interval
    uint32_t connect_timeout_ms;            ///< TCP connect timeout
    uint32_t response_timeout_ms;           ///< SCPI query response timeout
    uint8_t max_retries;                    ///< Max retry attempts per command
};

/// @brief Live measurement/status snapshot of one PSU channel
struct PSUChannelStatus {
    uint8_t channel;
    double measured_voltage_v;
    double measured_current_a;
    double measured_power_w;
    bool output_enabled;
    uint64_t timestamp_ms;
    bool data_valid;
    std::string error_message;
};

/// @brief Global system configuration
struct SystemConfig {
    std::vector<BranchConfig> branches;
    
    // Polling parameters
    uint32_t poll_interval_sec;      ///< Slow control poll interval (default: 300s = 5min)
    uint32_t eeprom_check_interval_sec; ///< EEPROM validation interval (default: 86400s = 24h)
    
    // Timeouts
    uint32_t modbus_timeout_ms;      ///< Modbus transaction timeout (default: 1000ms)
    uint32_t response_timeout_ms;    ///< Response wait timeout (default: 500ms)
    
    // Retry policy
    uint8_t max_retries;             ///< Max retry attempts (default: 3)
    uint32_t retry_delay_ms;         ///< Delay between retries (default: 100ms)

    // Power supply (optional; present() == !channels.empty())
    PSUConfig power_supply;          ///< OWON ODP6033 configuration
};

// =============================================================================
// Run Control
// =============================================================================

/// @brief DAQ run modes
enum class RunMode : uint8_t {
    STOPPED = 0,
    PHYSICS_RUN = 1,
    CALIBRATION_RUN = 2,
    MAINTENANCE = 3
};

/// @brief Run state information
struct RunState {
    RunMode mode;
    uint32_t run_number;
    bool transition_in_progress;
    std::string transition_target; ///< Target mode during transition
};

// =============================================================================
// Alarm Definitions
// =============================================================================

/// @brief Alarm severity levels
enum class AlarmSeverity : uint8_t {
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    CRITICAL = 3
};

/// @brief Alarm structure
struct Alarm {
    AlarmSeverity severity;
    std::string source;         ///< e.g., "Branch 0, Board PMT-01-01"
    std::string message;
    uint64_t timestamp_ms;
};

// =============================================================================
// Statistics
// =============================================================================

/// @brief Communication statistics per branch
struct BranchStats {
    uint64_t total_transactions;
    uint64_t successful_reads;
    uint64_t failed_reads;
    uint64_t timeouts;
    uint64_t crc_errors;
    double avg_response_time_ms;
};

} // namespace danss

#endif // DANSS_COMMON_TYPES_H
