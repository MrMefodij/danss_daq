#include "slowcontrol/exclusions_file.h"
#include "common/config_loader.h"
#include "common/constants.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace danss {

namespace {

uint16_t list_to_mask(const json& v, size_t bits, const std::string& where) {
    if (!v.is_array()) throw ConfigError(where + ": ожидается список номеров");
    uint16_t m = 0;
    for (const auto& x : v) {
        if (!x.is_number_integer() || x.get<int64_t>() < 0 || x.get<int64_t>() >= static_cast<int64_t>(bits))
            throw ConfigError(where + ": " + x.dump() + " — ожидается номер 0…" + std::to_string(bits - 1));
        m |= static_cast<uint16_t>(1u << x.get<int>());
    }
    return m;
}

} // namespace

ExclusionMap parse_exclusions(const std::string& text, const std::string& source) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error& e) {
        throw ConfigError(source + ": неверный JSON: " + e.what());
    }
    ExclusionMap out;
    if (!j.is_object()) throw ConfigError(source + ": ожидается объект { ... }");
    if (!j.contains("boards")) return out;
    const json& boards = j.at("boards");
    if (!boards.is_object()) throw ConfigError(source + ": boards: ожидается объект {\"имя платы\": {...}}");
    for (auto it = boards.begin(); it != boards.end(); ++it) {
        const std::string where = source + ": boards." + it.key();
        const json& b = it.value();
        if (!b.is_object()) throw ConfigError(where + ": ожидается объект { ... }");
        BoardExclusion e;
        if (b.contains("excluded_analog_boards"))
            e.masks.analog_boards = list_to_mask(b.at("excluded_analog_boards"), constants::NUM_ANALOG_BOARDS,
                                                 where + ".excluded_analog_boards");
        if (b.contains("excluded_counters"))
            e.masks.counters = list_to_mask(b.at("excluded_counters"), constants::NUM_SIPM_CHANNELS,
                                            where + ".excluded_counters");
        if (b.contains("reason") && b.at("reason").is_string()) e.reason = b.at("reason").get<std::string>();
        if (b.contains("changed") && b.at("changed").is_string()) e.changed = b.at("changed").get<std::string>();
        out[it.key()] = e;
    }
    return out;
}

ExclusionMap load_exclusions(const std::string& filename, bool& exists) {
    std::ifstream f(filename);
    exists = static_cast<bool>(f);
    if (!exists) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_exclusions(ss.str(), filename);
}

std::string exclusions_to_json(const ExclusionMap& map) {
    // Пишем руками: порядок полей и списки в одну строку — чтобы файл читался и diff в git был коротким
    auto list = [](uint16_t m, size_t bits) {
        std::string out = "[";
        for (size_t i = 0; i < bits; ++i)
            if ((m >> i) & 1) out += (out.size() > 1 ? ", " : "") + std::to_string(i);
        return out + "]";
    };
    std::string out = "{\n  \"description\": "
        + json("Ручные исключения состава плат SlowControl. Пишет danss_sc_fe при изменении "
               "/Equipment/SlowControl/Settings/Boards/<имя>/Exclude ...; руками править только "
               "при остановленном danss_sc_fe.").dump()
        + ",\n  \"boards\": {";
    bool first = true;
    for (const auto& [name, e] : map) {
        if (e.empty()) continue;
        out += std::string(first ? "" : ",") + "\n    " + json(name).dump() + ": {\n"
               + "      \"excluded_analog_boards\": " + list(e.masks.analog_boards, constants::NUM_ANALOG_BOARDS) + ",\n"
               + "      \"excluded_counters\": " + list(e.masks.counters, constants::NUM_SIPM_CHANNELS) + ",\n"
               + "      \"reason\": " + json(e.reason).dump() + ",\n"
               + "      \"changed\": " + json(e.changed).dump() + "\n    }";
        first = false;
    }
    out += first ? "}\n}\n" : "\n  }\n}\n";
    return out;
}

void save_exclusions(const std::string& filename, const ExclusionMap& map) {
    const std::string tmp = filename + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + tmp);
        f << exclusions_to_json(map);
        f.flush();
        if (!f) throw std::runtime_error("write error " + tmp);
    }
    if (std::rename(tmp.c_str(), filename.c_str()) != 0)
        throw std::runtime_error("cannot rename " + tmp + " -> " + filename);
}

} // namespace danss
