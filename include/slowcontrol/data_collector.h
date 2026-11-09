#ifndef DANSS_DATA_COLLECTOR_H
#define DANSS_DATA_COLLECTOR_H

// Разбор прочитанных регистров платы: что подключено, что исправно, что изменилось.
//
// Признаки — по письму Игоря 09.11.2026 (doc/MODBUS_MAP_v2.md, «Ответы Игоря»):
//  - счётчик (его сторона) подключён, если ID термометра (input 100–147) не нулевой;
//    температура 0x8001 — ошибка доступа к термометру счётчика;
//  - явного признака аналоговой платы нет: считаем её подключённой, если не все 32 обратных
//    чтения DAC (input 150–245) равны 0 (без платы читать нечего — там 0). При всех уставках
//    этой платы = 0 признак не работает — в реальной работе такие уставки маловероятны;
//  - в рабочем режиме должны быть все аналоговые платы и все счётчики. Что отсутствует и не
//    исключено оператором вручную (Exclusions) — проблема платы.

#include "common/types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace danss {
namespace collector {

/// Метка «нет доступа к термометру счётчика» в регистрах температуры SiPM
constexpr uint16_t TEMPERATURE_NO_SENSOR = constants::SIPM_TEMP_NO_ACCESS;

/// Минимально возможный цикл платы, мс — для отличия перезагрузки от переполнения счётчика.
/// Плата 17 при задержке 0 — 134 мс; берём с большим запасом.
constexpr double MIN_BOARD_CYCLE_MS = 50.0;

/// «Данные не обновляются» — только если счётчик стоит дольше этого (плюс 3 задержки цикла,
/// holding 99, считая её в мс). Два опроса подряд (например, после команды) могут попасть
/// в один цикл платы — это не остановка.
constexpr double STALE_MIN_MS = 1000.0;

constexpr size_t DAC_CHANNELS_PER_ANALOG_BOARD = constants::NUM_DAC_CHANNELS / constants::NUM_ANALOG_BOARDS;  // 32

bool temperature_valid(uint16_t raw);

/// Аналоговая плата (0–2) подключена: не все 32 обратных чтения DAC этой платы равны 0
bool analog_board_present(const BoardData& d, size_t analog_board);

/// Счётчик на канале 0–11 подключён: ID термометра прочитан и не нулевой
bool counter_present(const BoardData& d, size_t channel);

/// К какой аналоговой плате относится канал DAC (0–95): по 32 подряд
size_t analog_board_of_dac(size_t dac_channel);

/// Что оператор вручную исключил из проверки (бит на аналоговую плату / на канал счётчика)
struct Exclusions {
    uint16_t analog_boards = 0;
    uint16_t counters = 0;
};

// ---- Результат проверки ---------------------------------------------------------

struct BoardHealth {
    std::array<bool, constants::NUM_ANALOG_BOARDS> analog_present{};
    std::array<bool, constants::NUM_SIPM_CHANNELS> counter_present{};
    std::array<bool, constants::NUM_SIPM_CHANNELS> temperature_valid{};
    uint16_t missing_analog = 0;            ///< ожидаются (не исключены), но не видны
    uint16_t missing_counters = 0;          ///< ожидаются (не исключены), но ID = 0
    uint16_t temperature_errors = 0;        ///< счётчик есть, а температура 0x8001
    uint16_t tripped_boards = 0;            ///< биты t из input 1 — подключённые и не исключённые платы
    std::vector<size_t> dac_mismatches;     ///< каналы DAC подключённых и не исключённых плат
    bool fresh = false;                     ///< счётчик input 0 изменился с прошлого опроса
    bool rebooted = false;                  ///< счётчик сбросился — плата перезагружалась
    std::vector<std::string> problems;      ///< понятные человеку сообщения (для алармов)

    bool ok() const { return problems.empty(); }
    bool missing() const { return missing_analog || missing_counters || temperature_errors; }
    size_t analog_boards() const;
    size_t counters() const;
    uint16_t analog_mask() const;
    uint16_t counter_mask() const;
};

/// Проверить свежий снимок платы.
/// @param now   только что прочитанные данные (data_valid == true)
/// @param prev  предыдущий успешный снимок или nullptr (первый опрос)
/// @param elapsed_ms время между снимками
/// @param excluded что оператор исключил из проверки
/// @param unchanged_ms сколько счётчик input 0 уже не меняется (если не менялся с prev)
BoardHealth evaluate(const BoardData& now, const BoardData* prev, double elapsed_ms,
                     const Exclusions& excluded = {}, double unchanged_ms = 1e12);

/// "0, 2, 5-7" — номера установленных битов (для сообщений)
std::string mask_to_list(uint16_t mask, size_t bits);

} // namespace collector
} // namespace danss

#endif // DANSS_DATA_COLLECTOR_H
