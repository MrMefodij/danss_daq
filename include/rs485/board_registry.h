#ifndef DANSS_BOARD_REGISTRY_H
#define DANSS_BOARD_REGISTRY_H

#include "common/types.h"
#include "slowcontrol/data_collector.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace danss {

enum class BoardStatus : uint8_t {
    UNKNOWN = 0,   ///< ещё не опрашивалась
    ONLINE,        ///< отвечает, данные свежие
    STALE,         ///< отвечает, но счётчик input 0 не растёт — цикл платы стоит
    OFFLINE,       ///< не отвечает (после всех повторов) или порт ветки недоступен
    DISABLED       ///< enabled: false в конфиге — не опрашивается
};

const char* to_string(BoardStatus s);

/// Всё, что известно об одной плате
struct BoardState {
    uint8_t branch_id = 0;
    BoardConfig config;                 ///< из board_map.json (+ эталон EEPROM, если загружен)
    BoardStatus status = BoardStatus::UNKNOWN;
    BoardData data;                     ///< последний успешный снимок
    collector::BoardHealth health;      ///< проверка последнего снимка
    collector::Exclusions excluded;     ///< что оператор исключил из проверки (danss_sc_fe)
    uint64_t last_ok_ms = 0;            ///< время последнего ответа
    uint64_t last_attempt_ms = 0;
    uint32_t consecutive_failures = 0;
    uint64_t polls = 0, failures = 0, reboots = 0;
    std::string last_error;
};

/// Результат одного опроса платы (его готовит PeriodicPoller)
struct PollResult {
    bool ok = false;
    BoardData data;          ///< при ok — прочитанные данные
    std::string error;       ///< при !ok — причина
    uint64_t time_ms = 0;
};

/// @brief Реестр плат: конфигурация и текущее состояние каждой платы. Потокобезопасен.
///
/// Ключ платы — (ветка, адрес Modbus). Опрос сюда только пишет результаты; состояние
/// (ONLINE/STALE/OFFLINE), проверки, счётчики ошибок и перезагрузок вычисляются здесь же,
/// так что потребители (MIDAS frontend, интерлок HV) видят один согласованный снимок.
class BoardRegistry {
public:
    using Key = std::pair<uint8_t, uint8_t>;  // (branch id, address)

    explicit BoardRegistry(const SystemConfig& config);

    /// Подмешать эталоны EEPROM (по имени платы)
    void attach_eeprom_reference(const std::map<std::string, BoardConfig>& ref);

    /// Записать результат опроса; возвращает новое состояние платы
    BoardState update(uint8_t branch_id, uint8_t address, const PollResult& r);
    /// Ручные исключения (аналоговые платы, счётчики); действуют со следующего опроса.
    /// Возвращает false, если такой платы нет.
    bool set_exclusions(uint8_t branch_id, uint8_t address, const collector::Exclusions& ex);

    /// Порт ветки недоступен — все опрашиваемые платы ветки OFFLINE
    void mark_branch_offline(uint8_t branch_id, const std::string& error, uint64_t time_ms);

    std::optional<BoardState> get(uint8_t branch_id, uint8_t address) const;
    std::optional<BoardState> find(const std::string& name) const;
    std::vector<BoardState> snapshot() const;

    struct Summary {
        size_t total = 0, online = 0, stale = 0, offline = 0, disabled = 0, unknown = 0;
        size_t with_problems = 0, tripped = 0, with_missing = 0;
    };
    Summary summary() const;

    /// Все опрашиваемые платы на связи, данные свежие, проблем нет (trip, состав, DAC).
    /// Исключённое оператором вручную не считается.
    /// Кандидат в условие интерлока HV — само условие ещё не решено группой.
    bool all_enabled_healthy() const;

private:
    mutable std::mutex mutex_;
    std::map<Key, BoardState> boards_;
    uint32_t max_retries_ = 0;
};

} // namespace danss

#endif // DANSS_BOARD_REGISTRY_H
