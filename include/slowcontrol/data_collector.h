#ifndef DANSS_DATA_COLLECTOR_H
#define DANSS_DATA_COLLECTOR_H

// Разбор прочитанных регистров платы: что подключено, что исправно, что изменилось.
//
// Здесь собраны все места, которые зависят от ещё не подтверждённых деталей прошивки
// (письмо Игорю 07.11.2026). Когда придут ответы, меняются только функции этого файла;
// опрос (PeriodicPoller), состояние плат (BoardRegistry) и алармы остаются как есть.

#include "common/types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace danss {
namespace collector {

// ---- Допущения (вопросы 1–3 к Игорю) -----------------------------------------

/// Метка «датчика нет» в регистрах температуры SiPM.
/// ДОПУЩЕНИЕ: так плата 17 без термометров показывает все 12 каналов (07.11.2026).
constexpr uint16_t TEMPERATURE_NO_SENSOR = 0x8001;

/// Минимально возможный цикл платы, мс — для отличия перезагрузки от переполнения счётчика.
/// Плата 17 при задержке 0 — 134 мс; берём с большим запасом.
constexpr double MIN_BOARD_CYCLE_MS = 50.0;

constexpr size_t DAC_CHANNELS_PER_ANALOG_BOARD = constants::NUM_DAC_CHANNELS / constants::NUM_ANALOG_BOARDS;  // 32

bool temperature_valid(uint16_t raw);

/// Подключена ли аналоговая плата (0–2).
/// ДОПУЩЕНИЕ: подключена, если хотя бы у одного из её каналов SiPM температура не «датчика нет».
bool analog_board_present(const BoardData& d, size_t analog_board);

/// К какой аналоговой плате относится канал DAC (0–95).
/// ДОПУЩЕНИЕ: по 32 подряд (0–31 — плата 0, 32–63 — плата 1, 64–95 — плата 2).
size_t analog_board_of_dac(size_t dac_channel);

// ---- Результат проверки ---------------------------------------------------------

struct BoardHealth {
    std::array<bool, constants::NUM_ANALOG_BOARDS> analog_present{};
    std::array<bool, constants::NUM_SIPM_CHANNELS> temperature_valid{};
    uint16_t tripped_boards = 0;            ///< биты t из input 1 (только подключённые платы)
    std::vector<size_t> dac_mismatches;     ///< каналы DAC подключённых плат, где применено не то
    bool fresh = false;                     ///< счётчик input 0 изменился с прошлого опроса
    bool rebooted = false;                  ///< счётчик сбросился — плата перезагружалась
    std::vector<std::string> problems;      ///< понятные человеку сообщения (для алармов)

    bool ok() const { return problems.empty(); }
    size_t analog_boards() const;
};

/// Проверить свежий снимок платы.
/// @param now   только что прочитанные данные (data_valid == true)
/// @param prev  предыдущий успешный снимок или nullptr (первый опрос)
/// @param elapsed_ms время между снимками
BoardHealth evaluate(const BoardData& now, const BoardData* prev, double elapsed_ms);

} // namespace collector
} // namespace danss

#endif // DANSS_DATA_COLLECTOR_H
