#include "slowcontrol/setpoints.h"
#include "common/config_loader.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace danss {

using namespace constants;

BoardSetpoints BoardSetpoints::from_board(const BoardData& d) {
    BoardSetpoints s;
    s.dac = d.dac_settings;
    s.hv_enable = d.hv_enable;
    s.gain_trip_enable = d.gain_trip_enable;
    s.cycle_delay = d.cycle_delay;
    return s;
}

bool BoardSetpoints::operator==(const BoardSetpoints& o) const {
    return differs(o) == 0;
}

uint8_t BoardSetpoints::differs(const BoardSetpoints& o) const {
    uint8_t f = 0;
    if (dac != o.dac) f |= DAC;
    if (hv_enable != o.hv_enable) f |= HV_ENABLE;
    if (gain_trip_enable != o.gain_trip_enable) f |= GAIN_TRIP;
    if (cycle_delay != o.cycle_delay) f |= CYCLE_DELAY;
    return f;
}

std::string validate(const BoardSetpoints& s) {
    char b[160];
    if (s.hv_enable & ~HV_ENABLE_VALID_MASK) {
        std::snprintf(b, sizeof(b), "HV enable 0x%04X: only bits 0-11 (channels) are allowed (mask 0x%03X)", s.hv_enable,
                      HV_ENABLE_VALID_MASK);
        return b;
    }
    if (s.gain_trip_enable & ~GAIN_TRIP_VALID_MASK) {
        std::snprintf(b, sizeof(b), "gain/trip enable 0x%04X: only bits 0-2 and 8-10 are allowed (mask 0x%04X)",
                      s.gain_trip_enable, GAIN_TRIP_VALID_MASK);
        return b;
    }
    return "";
}

std::string field_names(uint8_t f) {
    std::string out;
    auto add = [&](const char* s) { out += (out.empty() ? "" : ", ") + std::string(s); };
    if (f & BoardSetpoints::DAC) add("DAC");
    if (f & BoardSetpoints::HV_ENABLE) add("HV enable");
    if (f & BoardSetpoints::GAIN_TRIP) add("gain/trip enable");
    if (f & BoardSetpoints::CYCLE_DELAY) add("cycle delay");
    return out;
}

std::string describe_diff(const BoardSetpoints& a, const BoardSetpoints& b, size_t max_dac) {
    std::string out;
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : "; ") + s; };
    size_t n = 0, shown = 0;
    std::string dac;
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) {
        if (a.dac[i] == b.dac[i]) continue;
        ++n;
        if (shown < max_dac) {
            dac += (dac.empty() ? "" : ", ") + std::to_string(i) + ": " + std::to_string(a.dac[i]) + " -> "
                   + std::to_string(b.dac[i]);
            ++shown;
        }
    }
    if (n) add("DAC " + dac + (n > shown ? " (+" + std::to_string(n - shown) + " more)" : ""));
    char buf[64];
    if (a.hv_enable != b.hv_enable) {
        std::snprintf(buf, sizeof(buf), "HV enable 0x%03X -> 0x%03X", a.hv_enable, b.hv_enable);
        add(buf);
    }
    if (a.gain_trip_enable != b.gain_trip_enable) {
        std::snprintf(buf, sizeof(buf), "gain/trip enable 0x%04X -> 0x%04X", a.gain_trip_enable, b.gain_trip_enable);
        add(buf);
    }
    if (a.cycle_delay != b.cycle_delay)
        add("cycle delay " + std::to_string(a.cycle_delay) + " -> " + std::to_string(b.cycle_delay));
    return out;
}

// ---- файл ----------------------------------------------------------------------

namespace {

uint16_t word(const json& b, const char* key, const std::string& where, uint16_t def = 0) {
    if (!b.contains(key)) return def;
    const json& v = b.at(key);
    if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() > 0xFFFF)
        throw ConfigError(where + "." + key + ": " + v.dump() + " — ожидается целое 0…65535");
    return static_cast<uint16_t>(v.get<int64_t>());
}

} // namespace

SetpointsMap parse_setpoints(const std::string& text, const std::string& source) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error& e) {
        throw ConfigError(source + ": неверный JSON: " + e.what());
    }
    SetpointsMap out;
    if (!j.is_object()) throw ConfigError(source + ": ожидается объект { ... }");
    if (!j.contains("boards")) return out;
    const json& boards = j.at("boards");
    if (!boards.is_object()) throw ConfigError(source + ": boards: ожидается объект {\"имя платы\": {...}}");
    for (auto it = boards.begin(); it != boards.end(); ++it) {
        const std::string where = source + ": boards." + it.key();
        const json& b = it.value();
        if (!b.is_object()) throw ConfigError(where + ": ожидается объект { ... }");
        SetpointsEntry e;
        if (!b.contains("dac") || !b.at("dac").is_array() || b.at("dac").size() != NUM_DAC_CHANNELS)
            throw ConfigError(where + ".dac: ожидается список из " + std::to_string(NUM_DAC_CHANNELS) + " чисел");
        for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) {
            const json& v = b.at("dac")[i];
            if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() > 0xFFFF)
                throw ConfigError(where + ".dac[" + std::to_string(i) + "]: " + v.dump() + " — ожидается целое 0…65535");
            e.sp.dac[i] = static_cast<uint16_t>(v.get<int64_t>());
        }
        e.sp.hv_enable = word(b, "hv_enable", where);
        e.sp.gain_trip_enable = word(b, "gain_trip_enable", where);
        e.sp.cycle_delay = word(b, "cycle_delay", where);
        const std::string bad = validate(e.sp);
        if (!bad.empty()) throw ConfigError(where + ": " + bad);
        if (b.contains("changed") && b.at("changed").is_string()) e.changed = b.at("changed").get<std::string>();
        if (b.contains("source") && b.at("source").is_string()) e.source = b.at("source").get<std::string>();
        out[it.key()] = e;
    }
    return out;
}

SetpointsMap load_setpoints(const std::string& filename, bool& exists) {
    std::ifstream f(filename);
    exists = static_cast<bool>(f);
    if (!exists) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_setpoints(ss.str(), filename);
}

std::string setpoints_to_json(const SetpointsMap& map) {
    // Пишем руками: DAC по 16 в строке, порядок полей постоянный — файл читается и diff короткий
    std::string out = "{\n  \"description\": "
        + json("Уставки плат SlowControl (holding 0-99). Пишет danss_sc_fe при изменении "
               "/Equipment/SlowControl/Settings/Boards/<имя>/ и по команде Read from board; руками "
               "править только при остановленном danss_sc_fe.").dump()
        + ",\n  \"boards\": {";
    bool first = true;
    for (const auto& [name, e] : map) {
        out += std::string(first ? "" : ",") + "\n    " + json(name).dump() + ": {\n      \"dac\": [";
        for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) {
            if (i % 16 == 0) out += "\n        ";
            out += std::to_string(e.sp.dac[i]);
            if (i + 1 < NUM_DAC_CHANNELS) out += (i % 16 == 15) ? "," : ", ";
        }
        out += "\n      ],\n";
        out += "      \"hv_enable\": " + std::to_string(e.sp.hv_enable) + ",\n";
        out += "      \"gain_trip_enable\": " + std::to_string(e.sp.gain_trip_enable) + ",\n";
        out += "      \"cycle_delay\": " + std::to_string(e.sp.cycle_delay) + ",\n";
        out += "      \"changed\": " + json(e.changed).dump() + ",\n";
        out += "      \"source\": " + json(e.source).dump() + "\n    }";
        first = false;
    }
    out += first ? "}\n}\n" : "\n  }\n}\n";
    return out;
}

void save_setpoints(const std::string& filename, const SetpointsMap& map) {
    const std::string tmp = filename + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + tmp);
        f << setpoints_to_json(map);
        f.flush();
        if (!f) throw std::runtime_error("write error " + tmp);
    }
    if (std::rename(tmp.c_str(), filename.c_str()) != 0)
        throw std::runtime_error("cannot rename " + tmp + " -> " + filename);
}

} // namespace danss
