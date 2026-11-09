#ifndef DANSS_EXCLUSIONS_FILE_H
#define DANSS_EXCLUSIONS_FILE_H

// Ручные исключения состава плат (аналоговые платы, счётчики) — JSON между запусками MIDAS.
//
// Файл пишет danss_sc_fe при каждом изменении в ODB (Settings/Boards/<имя>/Exclude ...);
// при старте frontend файл главнее ODB. Руками править — при остановленном danss_sc_fe.
//
// {
//   "description": "...",
//   "boards": {
//     "BENCH-17": {
//       "excluded_analog_boards": [0, 1, 2],
//       "excluded_counters": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
//       "reason": "стол: процессорная плата без периферии",
//       "changed": "2026-11-09 15:20:00"
//     }
//   }
// }
//
// Платы без исключений в файл не пишутся.

#include "slowcontrol/data_collector.h"

#include <map>
#include <string>

namespace danss {

struct BoardExclusion {
    collector::Exclusions masks;
    std::string reason;
    std::string changed;  ///< когда изменено (локальное время)

    bool empty() const { return masks.analog_boards == 0 && masks.counters == 0; }
};

/// Имя платы -> исключения
using ExclusionMap = std::map<std::string, BoardExclusion>;

/// Прочитать файл. Файла нет — пустая карта и exists = false.
/// @throws ConfigError — файл есть, но битый (неверный JSON, номер канала вне диапазона)
ExclusionMap load_exclusions(const std::string& filename, bool& exists);
ExclusionMap parse_exclusions(const std::string& json_text, const std::string& source);

/// Записать файл атомарно (временный файл + rename): при сбое старый файл остаётся целым.
/// @throws std::runtime_error
void save_exclusions(const std::string& filename, const ExclusionMap& map);
std::string exclusions_to_json(const ExclusionMap& map);

} // namespace danss

#endif // DANSS_EXCLUSIONS_FILE_H
