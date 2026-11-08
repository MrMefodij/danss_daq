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
    for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch)
        if (analog_board_of_channel(ch) == board && temperature_valid(d.sipm_channels[ch].temperature)) return true;
    return false;
}

size_t analog_board_of_dac(size_t dac_channel) {
    return std::min(dac_channel / DAC_CHANNELS_PER_ANALOG_BOARD, NUM_ANALOG_BOARDS - 1);
}

size_t BoardHealth::analog_boards() const {
    return static_cast<size_t>(std::count(analog_present.begin(), analog_present.end(), true));
}

BoardHealth evaluate(const BoardData& now, const BoardData* prev, double elapsed_ms) {
    BoardHealth h;

    for (size_t b = 0; b < NUM_ANALOG_BOARDS; ++b) h.analog_present[b] = analog_board_present(now, b);
    for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch)
        h.temperature_valid[ch] = h.analog_present[analog_board_of_channel(ch)]
                                  && temperature_valid(now.sipm_channels[ch].temperature);

    // Свежесть и перезагрузка по 16-битному счётчику циклов.
    // Переполнение (65535 -> 0) — нормальный ход: прирост по модулю 2^16 не больше, чем плата
    // могла сделать циклов за прошедшее время. Больше — счётчик начался заново, плата перезагрузилась.
    if (prev && prev->data_valid) {
        const uint16_t delta = static_cast<uint16_t>(now.update_counter - prev->update_counter);
        const double max_cycles = elapsed_ms / MIN_BOARD_CYCLE_MS + 10.0;
        h.fresh = delta != 0;
        h.rebooted = delta != 0 && delta > max_cycles;
        if (!h.fresh) h.problems.push_back("update counter did not change — board loop stopped (stale data)");
        if (h.rebooted) h.problems.push_back("update counter restarted — board was rebooted");
    } else {
        h.fresh = true;  // первый снимок — сравнивать не с чем
    }

    // Trip — только по подключённым аналоговым платам
    for (size_t b = 0; b < NUM_ANALOG_BOARDS; ++b)
        if (h.analog_present[b] && analog_board_tripped(now.hv_status, b)) h.tripped_boards |= static_cast<uint16_t>(1u << b);
    if (h.tripped_boards) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "HV trip on analog board(s) mask 0x%X", h.tripped_boards);
        h.problems.push_back(msg);
    }

    // DAC: без аналоговой платы обратное чтение всегда 0 — такие каналы не сверяем
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i)
        if (h.analog_present[analog_board_of_dac(i)] && now.dac_settings[i] != now.dac_readback[i])
            h.dac_mismatches.push_back(i);
    if (!h.dac_mismatches.empty())
        h.problems.push_back(std::to_string(h.dac_mismatches.size()) + " DAC channel(s): applied value differs from setpoint");

    return h;
}

} // namespace collector
} // namespace danss
