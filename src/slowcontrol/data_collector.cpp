#include "slowcontrol/data_collector.h"
#include "common/constants.h"

#include <algorithm>
#include <cstdio>

namespace danss {
namespace collector {

using namespace constants;

bool temperature_valid(uint16_t raw) {
    return raw != TEMPERATURE_NO_SENSOR;
}

bool analog_board_present(const BoardData& d, size_t board) {
    for (size_t i = board * DAC_CHANNELS_PER_ANALOG_BOARD; i < (board + 1) * DAC_CHANNELS_PER_ANALOG_BOARD; ++i)
        if (d.dac_readback[i] != 0) return true;
    return false;
}

bool counter_present(const BoardData& d, size_t channel) {
    const std::string& id = d.sipm_channels[channel].thermometer_id;
    return !id.empty() && id.find_first_not_of('0') != std::string::npos;
}

size_t analog_board_of_dac(size_t dac_channel) {
    return std::min(dac_channel / DAC_CHANNELS_PER_ANALOG_BOARD, NUM_ANALOG_BOARDS - 1);
}

std::string mask_to_list(uint16_t mask, size_t bits) {
    std::string out;
    for (size_t i = 0; i < bits;) {
        if (!((mask >> i) & 1)) { ++i; continue; }
        size_t j = i;
        while (j + 1 < bits && ((mask >> (j + 1)) & 1)) ++j;
        if (!out.empty()) out += ", ";
        out += std::to_string(i);
        if (j > i) out += (j == i + 1 ? ", " : "-") + std::to_string(j);
        i = j + 1;
    }
    return out;
}

size_t BoardHealth::analog_boards() const {
    return static_cast<size_t>(std::count(analog_present.begin(), analog_present.end(), true));
}

size_t BoardHealth::counters() const {
    return static_cast<size_t>(std::count(counter_present.begin(), counter_present.end(), true));
}

uint16_t BoardHealth::analog_mask() const {
    uint16_t m = 0;
    for (size_t i = 0; i < analog_present.size(); ++i) m |= analog_present[i] ? (1u << i) : 0;
    return m;
}

uint16_t BoardHealth::counter_mask() const {
    uint16_t m = 0;
    for (size_t i = 0; i < counter_present.size(); ++i) m |= counter_present[i] ? (1u << i) : 0;
    return m;
}

BoardHealth evaluate(const BoardData& now, const BoardData* prev, double elapsed_ms, const Exclusions& ex,
                     double unchanged_ms) {
    BoardHealth h;

    for (size_t b = 0; b < NUM_ANALOG_BOARDS; ++b) h.analog_present[b] = analog_board_present(now, b);
    for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch) {
        h.counter_present[ch] = counter_present(now, ch);
        h.temperature_valid[ch] = temperature_valid(now.sipm_channels[ch].temperature);
    }
    const uint16_t analog = h.analog_mask(), counters = h.counter_mask();
    uint16_t temp_ok = 0;
    for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch) temp_ok |= h.temperature_valid[ch] ? (1u << ch) : 0;
    const uint16_t use_analog = analog & ~ex.analog_boards & ALL_ANALOG_BOARDS_MASK;

    // Состав: всё, что не исключено вручную, должно быть на месте
    h.missing_analog = static_cast<uint16_t>(~analog & ~ex.analog_boards & ALL_ANALOG_BOARDS_MASK);
    h.missing_counters = static_cast<uint16_t>(~counters & ~ex.counters & ALL_COUNTERS_MASK);
    h.temperature_errors = static_cast<uint16_t>(counters & ~ex.counters & ~temp_ok & ALL_COUNTERS_MASK);
    if (h.missing_analog)
        h.problems.push_back("analog board(s) " + mask_to_list(h.missing_analog, NUM_ANALOG_BOARDS)
                             + " not seen (all DAC readbacks are 0)");
    if (h.missing_counters)
        h.problems.push_back("counter(s) on channel(s) " + mask_to_list(h.missing_counters, NUM_SIPM_CHANNELS)
                             + " not seen (thermometer ID = 0)");
    if (h.temperature_errors)
        h.problems.push_back("temperature unreadable on channel(s) "
                             + mask_to_list(h.temperature_errors, NUM_SIPM_CHANNELS));

    // Свежесть и перезагрузка по 16-битному счётчику циклов.
    // Переполнение (65535 -> 0) — нормальный ход: прирост по модулю 2^16 не больше, чем плата
    // могла сделать циклов за прошедшее время. Больше — счётчик начался заново, плата перезагрузилась.
    if (prev && prev->data_valid) {
        const uint16_t delta = static_cast<uint16_t>(now.update_counter - prev->update_counter);
        const double max_cycles = elapsed_ms / MIN_BOARD_CYCLE_MS + 10.0;
        // Счётчик стоит недолго (два опроса в одном цикле платы) — ещё не «остановка»
        h.fresh = delta != 0 || unchanged_ms < STALE_MIN_MS + 3.0 * now.cycle_delay;
        h.rebooted = delta != 0 && delta > max_cycles;
        if (!h.fresh) h.problems.push_back("update counter did not change — board loop stopped (stale data)");
        if (h.rebooted) h.problems.push_back("update counter restarted — board was rebooted");
    } else {
        h.fresh = true;  // первый снимок — сравнивать не с чем
    }

    // Trip — по подключённым и не исключённым аналоговым платам
    for (size_t b = 0; b < NUM_ANALOG_BOARDS; ++b)
        if (((use_analog >> b) & 1) && analog_board_tripped(now.hv_status, b))
            h.tripped_boards |= static_cast<uint16_t>(1u << b);
    if (h.tripped_boards) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "HV trip on analog board(s) mask 0x%X", h.tripped_boards);
        h.problems.push_back(msg);
    }

    // DAC: без аналоговой платы обратное чтение всегда 0 — её каналы не сверяем
    // (её отсутствие уже отмечено выше, если она не исключена)
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i)
        if (((use_analog >> analog_board_of_dac(i)) & 1) && now.dac_settings[i] != now.dac_readback[i])
            h.dac_mismatches.push_back(i);
    if (!h.dac_mismatches.empty())
        h.problems.push_back(std::to_string(h.dac_mismatches.size()) + " DAC channel(s): applied value differs from setpoint");

    return h;
}

} // namespace collector
} // namespace danss
