#ifndef DANSS_COMMON_CONSTANTS_H
#define DANSS_COMMON_CONSTANTS_H

#include <cstdint>
#include <cstddef>

namespace danss {
namespace constants {

// =============================================================================
// Modbus RTU Protocol
// =============================================================================

// Function codes
constexpr uint8_t MODBUS_FC_READ_HOLDING_REGISTERS = 0x03;
constexpr uint8_t MODBUS_FC_READ_INPUT_REGISTERS = 0x04;
constexpr uint8_t MODBUS_FC_WRITE_SINGLE_REGISTER = 0x06;
constexpr uint8_t MODBUS_FC_WRITE_MULTIPLE_REGISTERS = 0x10;

// Exception codes
constexpr uint8_t MODBUS_EXCEPTION_ILLEGAL_FUNCTION = 0x01;
constexpr uint8_t MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS = 0x02;
constexpr uint8_t MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE = 0x03;
constexpr uint8_t MODBUS_EXCEPTION_SLAVE_DEVICE_FAILURE = 0x04;

// Protocol limits
constexpr uint8_t MODBUS_MIN_ADDRESS = 1;
constexpr uint8_t MODBUS_MAX_ADDRESS = 247;
constexpr uint16_t MODBUS_MAX_REGISTERS = 125; // Per transaction
constexpr uint16_t MODBUS_MAX_WRITE_REGISTERS = 123; // Per write transaction (FC 0x10)

// Frame sizes
constexpr size_t MODBUS_RTU_MIN_FRAME_SIZE = 5; // Exception: address + FC + code + CRC(2)
constexpr size_t MODBUS_RTU_MAX_FRAME_SIZE = 256;

// Timing (per Modbus RTU specification)
constexpr uint32_t MODBUS_T15_CHAR_US = 750;  // 1.5 character times at 19200 baud
constexpr uint32_t MODBUS_T35_CHAR_US = 1750; // 3.5 character times at 19200 baud

// =============================================================================
// Board Register Map — v2 (прошивка процессорной платы, октябрь 2026)
// Полное описание: doc/MODBUS_MAP_v2.md
// =============================================================================

// ---- Input registers (FC 0x04, только чтение) ------------------------------

constexpr uint16_t REG_UPDATE_COUNTER = 0;     // растёт в конце каждого цикла обновления
constexpr uint16_t REG_HV_STATUS = 1;          // 0ttt eeee eeee eeee (см. ниже)
constexpr uint16_t REG_GAIN_TRIP_STATUS = 2;   // 0000 0rrr 0000 0aaa (см. ниже)
constexpr uint16_t REG_SIPM_CURRENT_START = 3;
constexpr uint16_t REG_SIPM_CURRENT_END = 14;
constexpr uint16_t REG_SIPM_TEMP_START = 15;
constexpr uint16_t REG_SIPM_TEMP_END = 26;
constexpr uint16_t REG_CPU_TEMPERATURE = 27;
// 28–99 — резерв

// Блок состояния: регистры 0–27 читаются одной транзакцией
constexpr uint16_t INPUT_STATUS_BLOCK_START = REG_UPDATE_COUNTER;
constexpr uint16_t INPUT_STATUS_BLOCK_COUNT = REG_CPU_TEMPERATURE - REG_UPDATE_COUNTER + 1; // 28

// EEPROM: ID термометров 1-Wire и CPU
constexpr uint16_t REG_THERMOMETER_ID_START = 100;
constexpr uint16_t REG_THERMOMETER_ID_END = 147;
constexpr uint16_t REG_THERMOMETER_ID_REGS_PER_SENSOR = 4; // 64-битный ID = 4 регистра
constexpr uint16_t REG_CPU_ID_START = 148;
constexpr uint16_t REG_CPU_ID_END = 149;

// Обратное чтение уставок DAC (то, что реально применено)
constexpr uint16_t REG_DAC_READBACK_START = 150;
constexpr uint16_t REG_DAC_READBACK_END = 245;

constexpr uint16_t REG_CPU_TEMP_OFFSET = 246;  // смещение датчика температуры CPU
// 247–249 — резерв

// Обратное чтение scratchpad (512 слов)
constexpr uint16_t REG_SCRATCHPAD_READBACK_START = 250;
constexpr uint16_t REG_SCRATCHPAD_READBACK_END = 761;

constexpr uint16_t REG_DEBUG_READ = 1000;

// ---- Holding registers (FC 0x03 / 0x06 / 0x10, чтение и запись) ------------

constexpr uint16_t REG_DAC_START = 0;
constexpr uint16_t REG_DAC_END = 95;
constexpr uint16_t REG_HV_ENABLE = 96;         // Counter ON/OFF = SiPM HV enable, бит на канал
constexpr uint16_t REG_GAIN_TRIP_ENABLE = 97;  // усиление и разрешение trip, бит на аналоговую плату
constexpr uint16_t REG_GLOBAL_CSR = 98;        // запись — команда, чтение — статус (CSR_*)
constexpr uint16_t REG_CYCLE_DELAY = 99;       // глобальная задержка между циклами
constexpr uint16_t REG_DEVICE_ADDRESS = 100;   // задаётся только при адресе MODBUS_FACTORY_ADDRESS
constexpr uint16_t REG_DEBUG_WRITE = 1000;

// Блок holding 0–100 читается одной транзакцией
constexpr uint16_t HOLDING_BLOCK_START = REG_DAC_START;
constexpr uint16_t HOLDING_BLOCK_COUNT = REG_DEVICE_ADDRESS - REG_DAC_START + 1; // 101

// ---- Global CSR (holding 98) -----------------------------------------------

// Команды (запись)
constexpr uint16_t CSR_CMD_NOP = 0;
constexpr uint16_t CSR_CMD_LOAD_FROM_SCRATCHPAD = 101;
constexpr uint16_t CSR_CMD_ERASE_SCRATCHPAD = 103;  // обязательно перед сохранением
constexpr uint16_t CSR_CMD_SAVE_TO_SCRATCHPAD = 105;

// Статус последней команды (чтение)
constexpr uint16_t CSR_STATUS_OK = 0;
constexpr uint16_t CSR_STATUS_UNKNOWN_COMMAND = 10;
constexpr uint16_t CSR_STATUS_ERROR = 20;

// ---- Scratchpad (энергонезависимая память, слова по 16 бит) -----------------

constexpr size_t SCRATCHPAD_SIZE_WORDS = 512;
constexpr uint16_t SCRATCHPAD_DAC_START = 0;        // 0–95: копия holding 0–95
constexpr uint16_t SCRATCHPAD_HV_ENABLE = 96;       // копия holding 96
constexpr uint16_t SCRATCHPAD_CYCLE_DELAY = 99;
constexpr uint16_t SCRATCHPAD_MODBUS_ADDRESS = 100;
constexpr uint16_t SCRATCHPAD_CHECKSUM_WORD = 511;
constexpr uint16_t SCRATCHPAD_CHECKSUM_VALUE = 0xAA55; // сумма всех 512 слов (mod 2^16)

// Заводской адрес новой платы; адрес меняется через REG_DEVICE_ADDRESS
constexpr uint8_t MODBUS_FACTORY_ADDRESS = 247;

// =============================================================================
// Board Configuration
// =============================================================================

constexpr size_t NUM_SIPM_CHANNELS = 12;
constexpr size_t NUM_DAC_CHANNELS = 96;
constexpr size_t NUM_THERMOMETERS = 12;
constexpr size_t NUM_ANALOG_BOARDS = 3;   // аналоговых плат на одну процессорную
// ПРЕДПОЛОЖЕНИЕ, не подтверждено Игорем: каналы идут по 4 подряд
// (0–3 — плата 0, 4–7 — плата 1, 8–11 — плата 2), бит 0 = плата 0 / канал 0.
constexpr size_t SIPM_CHANNELS_PER_ANALOG_BOARD = NUM_SIPM_CHANNELS / NUM_ANALOG_BOARDS;

// ---- Битовые поля ----------------------------------------------------------

// Input 1, HV status: 0ttt eeee eeee eeee
constexpr uint16_t HV_STATUS_ENABLE_MASK = 0x0FFF; // e — HV включено, бит на канал
constexpr unsigned HV_STATUS_TRIP_SHIFT = 12;      // t — trip, бит на аналоговую плату
constexpr uint16_t ANALOG_BOARD_BITS_MASK = 0x0007;

// Input 2, gain/trip status: 0000 0rrr 0000 0aaa
constexpr uint16_t GAIN_STATUS_MASK = 0x0007;      // a — усиление, бит на аналоговую плату
constexpr unsigned TRIP_ENABLE_SHIFT = 8;          // r — разрешение trip, бит на аналоговую плату
// Holding 97 — «бит на плату»; предполагаем ту же раскладку, что у input 2 (уточнить у Игоря).

constexpr bool hv_channel_enabled(uint16_t hv_status, size_t channel) {
    return (hv_status >> channel) & 1u;
}
constexpr uint16_t hv_enabled_channels(uint16_t hv_status) {
    return hv_status & HV_STATUS_ENABLE_MASK;
}
constexpr uint16_t tripped_analog_boards(uint16_t hv_status) {
    return (hv_status >> HV_STATUS_TRIP_SHIFT) & ANALOG_BOARD_BITS_MASK;
}
constexpr bool analog_board_tripped(uint16_t hv_status, size_t board) {
    return (tripped_analog_boards(hv_status) >> board) & 1u;
}
constexpr uint16_t gain_bits(uint16_t gain_trip) {
    return gain_trip & GAIN_STATUS_MASK;
}
constexpr uint16_t trip_enable_bits(uint16_t gain_trip) {
    return (gain_trip >> TRIP_ENABLE_SHIFT) & ANALOG_BOARD_BITS_MASK;
}
constexpr size_t analog_board_of_channel(size_t channel) {
    return channel / SIPM_CHANNELS_PER_ANALOG_BOARD;
}

static_assert(INPUT_STATUS_BLOCK_COUNT <= MODBUS_MAX_REGISTERS, "input block > 125");
static_assert(HOLDING_BLOCK_COUNT <= MODBUS_MAX_REGISTERS, "holding block > 125");
static_assert(REG_SIPM_CURRENT_END - REG_SIPM_CURRENT_START + 1 == NUM_SIPM_CHANNELS, "currents");
static_assert(REG_SIPM_TEMP_END - REG_SIPM_TEMP_START + 1 == NUM_SIPM_CHANNELS, "temperatures");
static_assert(REG_DAC_END - REG_DAC_START + 1 == NUM_DAC_CHANNELS, "DAC");
static_assert(REG_DAC_READBACK_END - REG_DAC_READBACK_START + 1 == NUM_DAC_CHANNELS, "DAC readback");
static_assert(REG_SCRATCHPAD_READBACK_END - REG_SCRATCHPAD_READBACK_START + 1 == SCRATCHPAD_SIZE_WORDS, "scratchpad");
static_assert(NUM_SIPM_CHANNELS % NUM_ANALOG_BOARDS == 0, "channels per analog board");

// =============================================================================
// System Configuration
// =============================================================================

// Default polling intervals
constexpr uint32_t DEFAULT_POLL_INTERVAL_SEC = 300;        // 5 minutes
constexpr uint32_t DEFAULT_EEPROM_CHECK_INTERVAL_SEC = 86400; // 24 hours

// Default timeouts
constexpr uint32_t DEFAULT_MODBUS_TIMEOUT_MS = 1000;
constexpr uint32_t DEFAULT_RESPONSE_TIMEOUT_MS = 500;

// Default retry policy
constexpr uint8_t DEFAULT_MAX_RETRIES = 3;
constexpr uint32_t DEFAULT_RETRY_DELAY_MS = 100;

// Serial port defaults
constexpr uint32_t DEFAULT_BAUD_RATE = 19200;
constexpr uint8_t DEFAULT_DATA_BITS = 8;
constexpr char DEFAULT_PARITY = 'N';
constexpr uint8_t DEFAULT_STOP_BITS = 1;

// System limits
constexpr size_t MAX_RS485_BRANCHES = 8;
constexpr size_t MAX_BOARDS_PER_BRANCH = 64; // Limited by reasonable polling time
constexpr size_t EXPECTED_TOTAL_BOARDS = 240;

// =============================================================================
// MIDAS Integration
// =============================================================================

// Equipment names
constexpr const char* EQUIPMENT_NAME_SLOW_CONTROL = "SlowControl";
constexpr const char* EQUIPMENT_NAME_SYNC = "RunSync";

// ODB paths
constexpr const char* ODB_PATH_SETTINGS = "/Equipment/SlowControl/Settings";
constexpr const char* ODB_PATH_VARIABLES = "/Equipment/SlowControl/Variables";
constexpr const char* ODB_PATH_ALARMS = "/Alarms/SlowControl";

// Event IDs
constexpr uint16_t EVENT_ID_SLOW_CONTROL = 1;
constexpr uint16_t EVENT_ID_BOARD_DATA = 2;
constexpr uint16_t EVENT_ID_EEPROM_CHECK = 3;

// Bank names (4 characters max)
constexpr const char* BANK_NAME_BOARD_DATA = "BDAT";
constexpr const char* BANK_NAME_STATISTICS = "STAT";
constexpr const char* BANK_NAME_ALARMS = "ALRM";

// =============================================================================
// Power Supply (OWON ODP6033 / SCPI over TCP)
// =============================================================================

// Default connection parameters
constexpr uint16_t DEFAULT_PSU_PORT = 3000;
constexpr uint32_t DEFAULT_PSU_CONNECT_TIMEOUT_MS = 2000;
constexpr uint32_t DEFAULT_PSU_RESPONSE_TIMEOUT_MS = 1000;
constexpr uint32_t DEFAULT_PSU_POLL_INTERVAL_SEC = 300;
constexpr uint8_t DEFAULT_PSU_MAX_RETRIES = 3;
constexpr size_t PSU_NUM_CHANNELS = 3;

// Default ramp behaviour for HV channels (SiPM bias)
constexpr double DEFAULT_PSU_RAMP_STEP_V = 1.0;
constexpr uint32_t DEFAULT_PSU_RAMP_INTERVAL_MS = 500;

// MIDAS integration
constexpr const char* EQUIPMENT_NAME_POWER_SUPPLY = "PowerSupply";

constexpr const char* ODB_PATH_PSU_SETTINGS = "/Equipment/PowerSupply/Settings";
constexpr const char* ODB_PATH_PSU_VARIABLES = "/Equipment/PowerSupply/Variables";
constexpr const char* ODB_PATH_PSU_ALARMS = "/Alarms/PowerSupply";

constexpr uint16_t EVENT_ID_POWER_SUPPLY = 4;

constexpr const char* BANK_NAME_PSU_DATA = "PSUD";

// =============================================================================
// Alarm Thresholds (example values - adjust as needed)
// =============================================================================

constexpr uint16_t SIPM_CURRENT_WARNING_THRESHOLD = 3000;  // ADC units
constexpr uint16_t SIPM_CURRENT_ERROR_THRESHOLD = 4000;
constexpr uint16_t SIPM_TEMP_WARNING_THRESHOLD = 3500;     // ADC units
constexpr uint16_t SIPM_TEMP_ERROR_THRESHOLD = 4000;
constexpr uint16_t CPU_TEMP_WARNING_THRESHOLD = 3000;
constexpr uint16_t CPU_TEMP_ERROR_THRESHOLD = 3500;

// Power supply alarm thresholds
constexpr double PSU_VOLTAGE_TOLERANCE_V = 0.5;  // acceptable deviation from setpoint before alarm
constexpr double PSU_OVP_MARGIN_V = 2.0;         // default OVP threshold = target + margin
constexpr double PSU_OCP_MARGIN_A = 0.2;         // default OCP threshold = current_limit + margin

// =============================================================================
// Logging
// =============================================================================

enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

} // namespace constants
} // namespace danss

#endif // DANSS_COMMON_CONSTANTS_H
