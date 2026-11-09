#include "rs485/board_registry.h"
#include "common/logger.h"

namespace danss {

const char* to_string(BoardStatus s) {
    switch (s) {
        case BoardStatus::UNKNOWN:  return "unknown";
        case BoardStatus::ONLINE:   return "online";
        case BoardStatus::STALE:    return "stale";
        case BoardStatus::OFFLINE:  return "offline";
        case BoardStatus::DISABLED: return "disabled";
    }
    return "?";
}

BoardRegistry::BoardRegistry(const SystemConfig& config) : max_retries_(config.max_retries) {
    for (const auto& br : config.branches) {
        for (const auto& b : br.boards) {
            BoardState s;
            s.branch_id = br.id;
            s.config = b;
            s.status = b.enabled ? BoardStatus::UNKNOWN : BoardStatus::DISABLED;
            s.data.modbus_address = b.address;
            s.data.board_name = b.name;
            boards_[{br.id, b.address}] = s;
        }
    }
}

void BoardRegistry::attach_eeprom_reference(const std::map<std::string, BoardConfig>& ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, s] : boards_) {
        auto it = ref.find(s.config.name);
        if (it == ref.end()) continue;
        s.config.thermometer_ids = it->second.thermometer_ids;
        s.config.cpu_id = it->second.cpu_id;
    }
}

BoardState BoardRegistry::update(uint8_t branch_id, uint8_t address, const PollResult& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = boards_.find({branch_id, address});
    if (it == boards_.end()) return BoardState{};
    BoardState& s = it->second;
    const BoardStatus before = s.status;
    s.polls++;
    s.last_attempt_ms = r.time_ms;

    if (!r.ok) {
        s.failures++;
        s.consecutive_failures++;
        s.last_error = r.error;
        s.status = BoardStatus::OFFLINE;  // PeriodicPoller уже сделал все повторы
    } else {
        const bool had = s.data.data_valid && s.last_ok_ms > 0;
        const double elapsed = had ? static_cast<double>(r.time_ms - s.last_ok_ms) : 0.0;
        const bool changed = !had || r.data.update_counter != s.data.update_counter;
        if (changed) s.last_change_ms = r.time_ms;
        const double unchanged = static_cast<double>(r.time_ms - s.last_change_ms);
        s.health = collector::evaluate(r.data, had ? &s.data : nullptr, elapsed, s.excluded, unchanged);
        if (s.health.rebooted) s.reboots++;
        s.data = r.data;
        s.data.board_name = s.config.name;
        s.data.data_fresh = s.health.fresh;
        s.last_ok_ms = r.time_ms;
        s.consecutive_failures = 0;
        s.last_error.clear();
        s.status = s.health.fresh ? BoardStatus::ONLINE : BoardStatus::STALE;
    }

    if (s.status != before) {
        std::string msg = "Board " + s.config.name + " (branch " + std::to_string(branch_id) + ", addr "
                          + std::to_string(address) + "): " + to_string(before) + " -> " + to_string(s.status);
        if (!r.ok) msg += " (" + r.error + ")";
        if (s.status == BoardStatus::ONLINE) LOG_INFO(msg);
        else LOG_WARNING(msg);
    }
    if (r.ok && s.health.rebooted) LOG_WARNING("Board " + s.config.name + ": rebooted (update counter restarted)");
    return s;
}

bool BoardRegistry::set_exclusions(uint8_t branch_id, uint8_t address, const collector::Exclusions& ex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = boards_.find({branch_id, address});
    if (it == boards_.end()) return false;
    it->second.excluded = ex;
    return true;
}

void BoardRegistry::mark_branch_offline(uint8_t branch_id, const std::string& error, uint64_t time_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, s] : boards_) {
        if (key.first != branch_id || s.status == BoardStatus::DISABLED) continue;
        if (s.status != BoardStatus::OFFLINE)
            LOG_WARNING("Board " + s.config.name + ": offline — " + error);
        s.status = BoardStatus::OFFLINE;
        s.last_error = error;
        s.last_attempt_ms = time_ms;
        s.consecutive_failures++;
        s.failures++;
        s.polls++;
    }
}

std::optional<BoardState> BoardRegistry::get(uint8_t branch_id, uint8_t address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = boards_.find({branch_id, address});
    if (it == boards_.end()) return std::nullopt;
    return it->second;
}

std::optional<BoardState> BoardRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, s] : boards_)
        if (s.config.name == name) return s;
    return std::nullopt;
}

std::vector<BoardState> BoardRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BoardState> out;
    out.reserve(boards_.size());
    for (const auto& [key, s] : boards_) out.push_back(s);
    return out;
}

BoardRegistry::Summary BoardRegistry::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Summary sm;
    for (const auto& [key, s] : boards_) {
        sm.total++;
        switch (s.status) {
            case BoardStatus::ONLINE: sm.online++; break;
            case BoardStatus::STALE: sm.stale++; break;
            case BoardStatus::OFFLINE: sm.offline++; break;
            case BoardStatus::DISABLED: sm.disabled++; break;
            case BoardStatus::UNKNOWN: sm.unknown++; break;
        }
        if (s.status == BoardStatus::ONLINE || s.status == BoardStatus::STALE) {
            if (!s.health.ok()) sm.with_problems++;
            if (s.health.tripped_boards) sm.tripped++;
            if (s.health.missing()) sm.with_missing++;
        }
    }
    return sm;
}

bool BoardRegistry::all_enabled_healthy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    bool any = false;
    for (const auto& [key, s] : boards_) {
        if (s.status == BoardStatus::DISABLED) continue;
        any = true;
        if (s.status != BoardStatus::ONLINE || !s.health.ok()) return false;
    }
    return any;
}

} // namespace danss
