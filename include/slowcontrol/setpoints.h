#ifndef DANSS_SETPOINTS_H
#define DANSS_SETPOINTS_H

// Уставки платы, которыми управляет danss_sc_fe (задача s2-07, решение Р12), и их файл.
//
// Желаемые уставки живут в ODB (/Equipment/SlowControl/Settings/Boards/<имя>/) и между
// запусками MIDAS — в JSON (system.setpoints_file). Фактические — holding 0–99 платы,
// читаются каждым циклом опроса.
//
// Файл:
// {
//   "description": "...",
//   "boards": {
//     "BENCH-17": {
//       "dac": [ 96 чисел, по 16 в строке ],
//       "hv_enable": 0,           // holding 96, бит на канал SiPM (0x0FFF — все 12)
//       "gain_trip_enable": 0,    // holding 97, 0000 0rrr 0000 0aaa, бит на аналоговую плату
//       "cycle_delay": 0,         // holding 99
//       "changed": "2026-11-09 15:20:00",
//       "source": "operator"      // кто задал: operator / read from board
//     }
//   }
// }

#include "common/constants.h"
#include "common/types.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace danss {

struct BoardSetpoints {
    std::array<uint16_t, constants::NUM_DAC_CHANNELS> dac{};
    uint16_t hv_enable = 0;
    uint16_t gain_trip_enable = 0;
    uint16_t cycle_delay = 0;

    /// Какие группы уставок отличаются / записываются
    enum Field : uint8_t { DAC = 1, HV_ENABLE = 2, GAIN_TRIP = 4, CYCLE_DELAY = 8, ALL = 15 };

    static BoardSetpoints from_board(const BoardData& d);
    bool operator==(const BoardSetpoints& o) const;
    bool operator!=(const BoardSetpoints& o) const { return !(*this == o); }
    /// Какие группы отличаются от o (маска Field)
    uint8_t differs(const BoardSetpoints& o) const;
};

/// Допустимые значения (holding 96 — 12 бит, holding 97 — маска 0x0707)
constexpr uint16_t HV_ENABLE_VALID_MASK = constants::HV_STATUS_ENABLE_MASK;  // 0x0FFF
constexpr uint16_t GAIN_TRIP_VALID_MASK = 0x0707;

/// "" — всё допустимо, иначе что не так
std::string validate(const BoardSetpoints& s);

/// Понятное человеку описание различий: "DAC 5: 100 -> 300; HV enable 0x000 -> 0x00F".
/// @param from было (или «на плате»), @param to стало (или «в ODB»); max_dac — сколько каналов DAC перечислить
std::string describe_diff(const BoardSetpoints& from, const BoardSetpoints& to, size_t max_dac = 8);

std::string field_names(uint8_t fields);

// ---- файл ----------------------------------------------------------------------

struct SetpointsEntry {
    BoardSetpoints sp;
    std::string changed;  ///< когда изменено (локальное время)
    std::string source;   ///< operator / read from board
};

/// Имя платы -> уставки
using SetpointsMap = std::map<std::string, SetpointsEntry>;

/// @throws ConfigError — файл есть, но битый
SetpointsMap load_setpoints(const std::string& filename, bool& exists);
SetpointsMap parse_setpoints(const std::string& json_text, const std::string& source);
/// Атомарно (временный файл + rename). @throws std::runtime_error
void save_setpoints(const std::string& filename, const SetpointsMap& map);
std::string setpoints_to_json(const SetpointsMap& map);

} // namespace danss

#endif // DANSS_SETPOINTS_H
