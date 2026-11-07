#include "modbus/board_client.h"
#include "common/constants.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace danss;
using namespace danss::constants;

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count());
}

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

std::string danss::format_thermometer_id(const uint16_t* r) {
    char b[17];
    std::snprintf(b, sizeof(b), "%04X%04X%04X%04X", r[0], r[1], r[2], r[3]);
    return b;
}

std::string danss::format_cpu_id(const uint16_t* r) {
    char b[9];
    std::snprintf(b, sizeof(b), "%04X%04X", r[0], r[1]);
    return b;
}

bool BoardClient::fail(const std::string& what) {
    error_ = "board " + std::to_string(address_) + ": " + what;
    return false;
}

bool BoardClient::read_input(uint16_t start, uint16_t count, std::vector<uint16_t>& v) {
    if (!bus_.read_input_registers(address_, start, count, v) || v.size() != count)
        return fail("read input " + std::to_string(start) + "+" + std::to_string(count) + ": " + bus_.get_last_error());
    return true;
}

bool BoardClient::read_holding(uint16_t start, uint16_t count, std::vector<uint16_t>& v) {
    if (!bus_.read_holding_registers(address_, start, count, v) || v.size() != count)
        return fail("read holding " + std::to_string(start) + "+" + std::to_string(count) + ": " + bus_.get_last_error());
    return true;
}

// =============================================================================
// Чтение
// =============================================================================

bool BoardClient::read_status(BoardData& d) {
    std::vector<uint16_t> v;
    d.modbus_address = address_;
    d.timestamp_ms = now_ms();
    if (!read_input(INPUT_STATUS_BLOCK_START, INPUT_STATUS_BLOCK_COUNT, v)) {
        d.data_valid = false;
        d.error_message = error_;
        return false;
    }
    const uint16_t prev = d.update_counter;
    const bool had_data = d.data_valid;
    d.update_counter = v[REG_UPDATE_COUNTER];
    d.hv_status = v[REG_HV_STATUS];
    d.gain_trip_status = v[REG_GAIN_TRIP_STATUS];
    for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch) {
        d.sipm_channels[ch].leakage_current = v[REG_SIPM_CURRENT_START + ch];
        d.sipm_channels[ch].temperature = v[REG_SIPM_TEMP_START + ch];
    }
    d.cpu_temperature = v[REG_CPU_TEMPERATURE];
    d.data_fresh = !had_data || d.update_counter != prev;
    d.data_valid = true;
    d.error_message.clear();
    return true;
}

bool BoardClient::read_settings(BoardData& d) {
    std::vector<uint16_t> v;
    if (!read_holding(HOLDING_BLOCK_START, HOLDING_BLOCK_COUNT, v)) return false;
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) d.dac_settings[i] = v[REG_DAC_START + i];
    d.hv_enable = v[REG_HV_ENABLE];
    d.gain_trip_enable = v[REG_GAIN_TRIP_ENABLE];
    d.global_csr = v[REG_GLOBAL_CSR];
    d.cycle_delay = v[REG_CYCLE_DELAY];
    d.device_address = v[REG_DEVICE_ADDRESS];
    return true;
}

bool BoardClient::read_dac_readback(BoardData& d) {
    std::vector<uint16_t> v;
    if (!read_input(REG_DAC_READBACK_START, NUM_DAC_CHANNELS, v)) return false;
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) d.dac_readback[i] = v[i];
    return true;
}

bool BoardClient::read_ids(BoardData& d) {
    std::vector<uint16_t> v;
    const uint16_t count = REG_CPU_ID_END - REG_THERMOMETER_ID_START + 1;  // 100..149
    if (!read_input(REG_THERMOMETER_ID_START, count, v)) return false;
    for (size_t t = 0; t < NUM_THERMOMETERS; ++t)
        d.sipm_channels[t].thermometer_id = format_thermometer_id(&v[t * REG_THERMOMETER_ID_REGS_PER_SENSOR]);
    d.cpu_id = format_cpu_id(&v[REG_CPU_ID_START - REG_THERMOMETER_ID_START]);
    std::vector<uint16_t> off;
    if (!read_input(REG_CPU_TEMP_OFFSET, 1, off)) return false;
    d.cpu_temp_offset = off[0];
    return true;
}

bool BoardClient::read_scratchpad(ScratchpadImage& sp) {
    for (size_t pos = 0; pos < SCRATCHPAD_SIZE_WORDS;) {
        const uint16_t n = static_cast<uint16_t>(std::min<size_t>(MODBUS_MAX_REGISTERS, SCRATCHPAD_SIZE_WORDS - pos));
        std::vector<uint16_t> v;
        if (!read_input(static_cast<uint16_t>(REG_SCRATCHPAD_READBACK_START + pos), n, v)) return false;
        std::copy(v.begin(), v.end(), sp.words.begin() + static_cast<long>(pos));
        pos += n;
    }
    return true;
}

bool BoardClient::read_update_counter(uint16_t& out) {
    std::vector<uint16_t> v;
    if (!read_input(REG_UPDATE_COUNTER, 1, v)) return false;
    out = v[0];
    return true;
}

// =============================================================================
// Запись
// =============================================================================

bool BoardClient::write_dac(size_t channel, uint16_t value) {
    if (channel >= NUM_DAC_CHANNELS) return fail("DAC channel " + std::to_string(channel) + " out of range 0-95");
    if (!bus_.write_single_register(address_, static_cast<uint16_t>(REG_DAC_START + channel), value))
        return fail("write DAC " + std::to_string(channel) + ": " + bus_.get_last_error());
    return true;
}

bool BoardClient::write_all_dac(const std::array<uint16_t, NUM_DAC_CHANNELS>& values) {
    // 96 регистров > 123 нельзя? Можно: 96 ≤ MODBUS_MAX_WRITE_REGISTERS — одна транзакция
    std::vector<uint16_t> v(values.begin(), values.end());
    if (!bus_.write_multiple_registers(address_, REG_DAC_START, v))
        return fail("write DAC 0-95: " + bus_.get_last_error());
    return true;
}

bool BoardClient::write_hv_enable(uint16_t mask) {
    if (!bus_.write_single_register(address_, REG_HV_ENABLE, mask)) return fail("write HV enable: " + bus_.get_last_error());
    return true;
}

bool BoardClient::write_gain_trip_enable(uint16_t v) {
    if (!bus_.write_single_register(address_, REG_GAIN_TRIP_ENABLE, v))
        return fail("write gain/trip enable: " + bus_.get_last_error());
    return true;
}

bool BoardClient::write_cycle_delay(uint16_t v) {
    if (!bus_.write_single_register(address_, REG_CYCLE_DELAY, v)) return fail("write cycle delay: " + bus_.get_last_error());
    return true;
}

bool BoardClient::wait_next_cycle(double timeout_s, double* waited_s) {
    const auto t0 = std::chrono::steady_clock::now();
    uint16_t c0 = 0, c = 0;
    bool have = false;
    while (since(t0) < timeout_s) {
        if (read_update_counter(have ? c : c0)) {
            if (!have) have = true;
            else if (c != c0) {
                if (waited_s) *waited_s = since(t0);
                return true;
            }
        }
        sleep_ms(50);
    }
    return fail(have ? "update counter did not change within " + std::to_string(timeout_s) + " s (board stuck?)"
                     : "no answer within " + std::to_string(timeout_s) + " s");
}

bool BoardClient::write_dac_verified(const std::array<uint16_t, NUM_DAC_CHANNELS>& values,
                                     std::vector<size_t>& mismatches, double timeout_s) {
    mismatches.clear();
    if (!write_all_dac(values)) return false;
    // Ждём два цикла: запись могла попасть в середину текущего
    if (!wait_next_cycle(timeout_s) || !wait_next_cycle(timeout_s)) return false;
    BoardData d;
    if (!read_dac_readback(d)) return false;
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i)
        if (d.dac_readback[i] != values[i]) mismatches.push_back(i);
    if (!mismatches.empty())
        return fail(std::to_string(mismatches.size()) + " DAC channels: readback differs from written value");
    return true;
}

// =============================================================================
// CSR и scratchpad
// =============================================================================

bool BoardClient::csr_command(uint16_t command, uint16_t& status, double timeout_s) {
    if (!bus_.write_single_register(address_, REG_GLOBAL_CSR, command))
        return fail("CSR command " + std::to_string(command) + ": " + bus_.get_last_error());
    // Статус читаем после следующего цикла платы: команда выполняется в цикле.
    // Пока идёт запись во flash, плата может не отвечать — wait_next_cycle повторяет запросы.
    if (!wait_next_cycle(timeout_s)) return fail("CSR command " + std::to_string(command) + ": " + error_);
    std::vector<uint16_t> v;
    const auto t0 = std::chrono::steady_clock::now();
    while (!read_holding(REG_GLOBAL_CSR, 1, v)) {
        if (since(t0) > timeout_s) return false;
        sleep_ms(100);
    }
    status = v[0];
    if (status == CSR_STATUS_OK) return true;
    return fail("CSR command " + std::to_string(command) + " -> status " + std::to_string(status)
                + (status == CSR_STATUS_UNKNOWN_COMMAND ? " (unknown command)"
                   : status == CSR_STATUS_ERROR       ? " (error)" : ""));
}

bool BoardClient::erase_scratchpad() {
    uint16_t st = 0;
    return csr_command(CSR_CMD_ERASE_SCRATCHPAD, st);
}

bool BoardClient::load_from_scratchpad() {
    uint16_t st = 0;
    return csr_command(CSR_CMD_LOAD_FROM_SCRATCHPAD, st);
}

bool BoardClient::save_to_scratchpad(std::string& report) {
    BoardData d;
    if (!read_settings(d)) return false;
    uint16_t st = 0;
    if (!csr_command(CSR_CMD_ERASE_SCRATCHPAD, st)) return false;
    if (!csr_command(CSR_CMD_SAVE_TO_SCRATCHPAD, st)) return false;

    ScratchpadImage sp;
    if (!read_scratchpad(sp)) return false;
    std::string problems;
    if (!sp.checksum_ok()) {
        char b[64];
        std::snprintf(b, sizeof(b), "checksum 0x%04X != 0xAA55; ", sp.sum());
        problems += b;
    }
    size_t bad_dac = 0;
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) bad_dac += sp.dac(i) != d.dac_settings[i];
    if (bad_dac) problems += std::to_string(bad_dac) + " DAC words differ; ";
    if (sp.hv_enable() != d.hv_enable) problems += "HV enable differs; ";
    if (sp.cycle_delay() != d.cycle_delay) problems += "cycle delay differs; ";
    if (sp.modbus_address() != address_) problems += "address word " + std::to_string(sp.modbus_address()) + "; ";
    if (!problems.empty()) return fail("scratchpad after save: " + problems);
    report = "saved and verified: checksum 0xAA55, DAC 0-95, HV enable, cycle delay, address "
             + std::to_string(address_);
    return true;
}

bool BoardClient::assign_address(uint8_t new_address, std::string& report) {
    if (address_ != MODBUS_FACTORY_ADDRESS)
        return fail("address can be assigned only to a board at factory address 247 "
                    "(erase scratchpad, power-cycle the board, then assign)");
    if (new_address < MODBUS_MIN_ADDRESS || new_address >= MODBUS_FACTORY_ADDRESS)
        return fail("new address " + std::to_string(new_address) + " out of range 1-246");
    if (!bus_.write_single_register(address_, REG_DEVICE_ADDRESS, new_address))
        return fail("write address: " + bus_.get_last_error());
    // Сохранить всё (стирание обязательно перед сохранением)
    uint16_t st = 0;
    if (!csr_command(CSR_CMD_ERASE_SCRATCHPAD, st) || !csr_command(CSR_CMD_SAVE_TO_SCRATCHPAD, st)) return false;

    // Отвечает ли плата уже по новому адресу?
    BoardClient probe(bus_, new_address);
    uint16_t c = 0;
    if (probe.read_update_counter(c)) {
        address_ = new_address;
        report = "address " + std::to_string(new_address) + " assigned and active";
    } else {
        report = "address " + std::to_string(new_address)
                 + " saved to scratchpad; board still answers at 247 — power-cycle it to apply";
    }
    return true;
}
