#include "common/config_loader.h"
#include "common/constants.h"
#include "common/logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

using json = nlohmann::json;
using namespace danss;
using namespace danss::constants;

namespace {

std::string fmt(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

/// Узел JSON с путём до него — чтобы любая ошибка называла файл и поле.
class Node {
public:
    Node(const json& j, std::string path, const std::string& source)
        : j_(j), path_(std::move(path)), source_(source) {}

    const std::string& path() const { return path_; }

    std::string where(const std::string& key = "") const {
        if (key.empty()) return path_.empty() ? "<корень>" : path_;
        return path_.empty() ? key : path_ + "." + key;
    }

    [[noreturn]] void fail(const std::string& key, const std::string& msg) const {
        throw ConfigError(source_ + ": " + where(key) + ": " + msg);
    }

    void warn(const std::string& key, const std::string& msg) const {
        LOG_WARNING("Config " + source_ + ": " + where(key) + ": " + msg);
    }

    bool has(const std::string& key) const { return j_.contains(key) && !j_.at(key).is_null(); }

    Node object(const std::string& key) const {
        if (!has(key)) fail(key, "обязательный раздел отсутствует");
        const json& v = j_.at(key);
        if (!v.is_object()) fail(key, "ожидается объект { ... }");
        return Node(v, where(key), source_);
    }

    std::vector<Node> objects(const std::string& key) const {
        if (!has(key)) fail(key, "обязательный список отсутствует");
        const json& v = j_.at(key);
        if (!v.is_array()) fail(key, "ожидается список [ ... ]");
        std::vector<Node> out;
        for (size_t i = 0; i < v.size(); ++i) {
            std::string p = where(key) + "[" + std::to_string(i) + "]";
            if (!v[i].is_object()) throw ConfigError(source_ + ": " + p + ": ожидается объект { ... }");
            out.emplace_back(v[i], p, source_);
        }
        return out;
    }

    int64_t integer(const std::string& key, int64_t lo, int64_t hi,
                    std::optional<int64_t> def = std::nullopt) const {
        if (!has(key)) {
            if (def) return *def;
            fail(key, "обязательное поле отсутствует");
        }
        const json& v = j_.at(key);
        if (!v.is_number_integer()) fail(key, "ожидается целое число, получено " + v.dump());
        int64_t x = v.get<int64_t>();
        if (x < lo || x > hi)
            fail(key, std::to_string(x) + " вне допустимого диапазона " + std::to_string(lo) + "…"
                          + std::to_string(hi));
        return x;
    }

    double number(const std::string& key, double lo, double hi,
                  std::optional<double> def = std::nullopt) const {
        if (!has(key)) {
            if (def) return *def;
            fail(key, "обязательное поле отсутствует");
        }
        const json& v = j_.at(key);
        if (!v.is_number()) fail(key, "ожидается число, получено " + v.dump());
        double x = v.get<double>();
        if (x < lo || x > hi) fail(key, fmt(x) + " вне допустимого диапазона " + fmt(lo) + "…" + fmt(hi));
        return x;
    }

    bool boolean(const std::string& key, std::optional<bool> def = std::nullopt) const {
        if (!has(key)) {
            if (def) return *def;
            fail(key, "обязательное поле отсутствует");
        }
        const json& v = j_.at(key);
        if (!v.is_boolean()) fail(key, "ожидается true или false, получено " + v.dump());
        return v.get<bool>();
    }

    std::string string(const std::string& key, std::optional<std::string> def = std::nullopt) const {
        if (!has(key)) {
            if (def) return *def;
            fail(key, "обязательное поле отсутствует");
        }
        const json& v = j_.at(key);
        if (!v.is_string()) fail(key, "ожидается строка, получено " + v.dump());
        std::string s = v.get<std::string>();
        if (s.empty()) fail(key, "пустая строка");
        return s;
    }

    std::vector<std::string> strings(const std::string& key) const {
        if (!has(key)) fail(key, "обязательный список отсутствует");
        const json& v = j_.at(key);
        if (!v.is_array()) fail(key, "ожидается список строк");
        std::vector<std::string> out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (!v[i].is_string())
                fail(key, "элемент [" + std::to_string(i) + "]: ожидается строка, получено " + v[i].dump());
            out.push_back(v[i].get<std::string>());
        }
        return out;
    }

    /// Неизвестные поля — предупреждение (почти всегда опечатка: "ovp_limit" вместо "ovp_limit_v")
    void warn_unknown(std::initializer_list<const char*> known) const {
        for (auto it = j_.begin(); it != j_.end(); ++it) {
            bool ok = false;
            for (const char* k : known) ok = ok || it.key() == k;
            if (!ok) warn(it.key(), "неизвестное поле — проигнорировано (опечатка?)");
        }
    }

private:
    const json& j_;
    std::string path_;
    const std::string& source_;
};

json parse_text(const std::string& text, const std::string& source) {
    try {
        json j = json::parse(text);
        if (!j.is_object()) throw ConfigError(source + ": корень файла должен быть объектом { ... }");
        return j;
    } catch (const json::parse_error& e) {
        throw ConfigError(source + ": некорректный JSON: " + e.what());
    }
}

std::string read_file(const std::string& filename) {
    std::ifstream in(filename);
    if (!in) throw ConfigError(filename + ": не удалось открыть файл");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_hex(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------
// power_supply
// ---------------------------------------------------------------------------

PSUChannelRole parse_role(const Node& ch) {
    const std::string r = ch.string("role");
    if (r == "CPU_POWER") return PSUChannelRole::CPU_POWER;
    if (r == "SIPM_HV") return PSUChannelRole::SIPM_HV;
    if (r == "UNUSED") return PSUChannelRole::UNUSED;
    ch.fail("role", "\"" + r + "\" — допустимо CPU_POWER, SIPM_HV или UNUSED");
}

PSUChannelConfig parse_psu_channel(const Node& ch) {
    ch.warn_unknown({"channel", "role", "name", "target_voltage_v", "current_limit_a", "ovp_limit_v",
                     "ocp_limit_a", "ramp_step_v", "ramp_interval_ms", "require_interlock"});
    PSUChannelConfig c{};
    c.channel = static_cast<uint8_t>(ch.integer("channel", 1, static_cast<int64_t>(PSU_NUM_CHANNELS)));
    c.role = parse_role(ch);
    c.name = ch.string("name", "ch" + std::to_string(c.channel));

    const bool ch3 = (c.channel == 3);
    const double max_v = ch3 ? PSU_CH3_MAX_V : PSU_CH12_MAX_V;
    const double max_ovp = ch3 ? PSU_CH3_MAX_OVP_V : PSU_CH12_MAX_OVP_V;

    if (c.role == PSUChannelRole::UNUSED) {
        // Значения не используются; читаем, только чтобы поймать неверные типы
        c.target_voltage_v = ch.number("target_voltage_v", 0.0, max_v, 0.0);
        c.current_limit_a = ch.number("current_limit_a", 0.0, PSU_MAX_CURRENT_A, 0.0);
        c.ovp_limit_v = ch.number("ovp_limit_v", 0.0, max_ovp, 0.0);
        c.ocp_limit_a = ch.number("ocp_limit_a", 0.0, PSU_MAX_OCP_A, 0.0);
        c.ramp_step_v = ch.number("ramp_step_v", 0.0, max_v, DEFAULT_PSU_RAMP_STEP_V);
        c.ramp_interval_ms = static_cast<uint32_t>(ch.integer("ramp_interval_ms", 0, 600000,
                                                              DEFAULT_PSU_RAMP_INTERVAL_MS));
        c.require_interlock = ch.boolean("require_interlock", false);
        return c;
    }

    c.target_voltage_v = ch.number("target_voltage_v", 0.0, max_v);
    c.current_limit_a = ch.number("current_limit_a", 0.001, PSU_MAX_CURRENT_A);
    c.ovp_limit_v = ch.number("ovp_limit_v", 0.0, max_ovp);
    c.ocp_limit_a = ch.number("ocp_limit_a", 0.001, PSU_MAX_OCP_A);
    c.ramp_step_v = ch.number("ramp_step_v", 0.01, 10.0, DEFAULT_PSU_RAMP_STEP_V);
    c.ramp_interval_ms = static_cast<uint32_t>(ch.integer("ramp_interval_ms", 10, 600000,
                                                          DEFAULT_PSU_RAMP_INTERVAL_MS));
    c.require_interlock = ch.boolean("require_interlock", c.role == PSUChannelRole::SIPM_HV);

    // OVP должен быть выше цели с запасом: иначе защита сработает на шумах при разгоне
    if (c.ovp_limit_v < c.target_voltage_v + PSU_VOLTAGE_TOLERANCE_V)
        ch.fail("ovp_limit_v", fmt(c.ovp_limit_v) + " В — должно быть не ниже target_voltage_v + "
                                   + fmt(PSU_VOLTAGE_TOLERANCE_V) + " = "
                                   + fmt(c.target_voltage_v + PSU_VOLTAGE_TOLERANCE_V) + " В");
    if (c.ocp_limit_a < c.current_limit_a)
        ch.fail("ocp_limit_a", fmt(c.ocp_limit_a) + " А — должно быть не ниже current_limit_a = "
                                   + fmt(c.current_limit_a) + " А");

    if (c.role == PSUChannelRole::SIPM_HV) {
        if (!c.require_interlock)
            ch.fail("require_interlock", "для SIPM_HV интерлок обязателен (true)");
        if (c.ramp_step_v > 5.0)
            ch.fail("ramp_step_v", fmt(c.ramp_step_v) + " В — для SIPM_HV шаг разгона не больше 5 В");
    }
    return c;
}

PSUConfig parse_psu(const Node& ps) {
    ps.warn_unknown({"host", "port", "poll_interval_sec", "connect_timeout_ms", "response_timeout_ms",
                     "max_retries", "channels"});
    PSUConfig cfg;
    cfg.host = ps.string("host");
    cfg.port = static_cast<uint16_t>(ps.integer("port", 1, 65535, DEFAULT_PSU_PORT));
    cfg.poll_interval_sec = static_cast<uint32_t>(ps.integer("poll_interval_sec", 1, 86400,
                                                             DEFAULT_PSU_POLL_INTERVAL_SEC));
    cfg.connect_timeout_ms = static_cast<uint32_t>(ps.integer("connect_timeout_ms", 100, 60000,
                                                              DEFAULT_PSU_CONNECT_TIMEOUT_MS));
    cfg.response_timeout_ms = static_cast<uint32_t>(ps.integer("response_timeout_ms", 100, 60000,
                                                               DEFAULT_PSU_RESPONSE_TIMEOUT_MS));
    cfg.max_retries = static_cast<uint8_t>(ps.integer("max_retries", 0, 20, DEFAULT_PSU_MAX_RETRIES));

    std::set<uint8_t> seen;
    bool has_hv = false, has_cpu = false;
    for (const Node& n : ps.objects("channels")) {
        PSUChannelConfig c = parse_psu_channel(n);
        if (!seen.insert(c.channel).second)
            n.fail("channel", "канал " + std::to_string(c.channel) + " описан дважды");
        has_hv = has_hv || c.role == PSUChannelRole::SIPM_HV;
        has_cpu = has_cpu || c.role == PSUChannelRole::CPU_POWER;
        cfg.channels.push_back(c);
    }
    if (cfg.channels.empty()) ps.fail("channels", "список каналов пуст");
    if (has_hv && !has_cpu)
        ps.warn("channels", "есть SIPM_HV, но нет CPU_POWER — HV не будет проверять, включено ли питание электроники");
    return cfg;
}

} // namespace

// =============================================================================
// board_map.json
// =============================================================================

SystemConfig ConfigLoader::parse_system_config(const std::string& text, const std::string& source) {
    const json j = parse_text(text, source);
    const Node root(j, "", source);
    root.warn_unknown({"description", "system", "branches", "power_supply"});

    SystemConfig cfg;
    if (root.has("system")) {
        const Node s = root.object("system");
        s.warn_unknown({"poll_interval_sec", "eeprom_check_interval_sec", "modbus_timeout_ms",
                        "response_timeout_ms", "max_retries", "retry_delay_ms"});
        cfg.poll_interval_sec = static_cast<uint32_t>(s.integer("poll_interval_sec", 1, 86400, DEFAULT_POLL_INTERVAL_SEC));
        cfg.eeprom_check_interval_sec = static_cast<uint32_t>(
            s.integer("eeprom_check_interval_sec", 60, 7 * 86400, DEFAULT_EEPROM_CHECK_INTERVAL_SEC));
        cfg.modbus_timeout_ms = static_cast<uint32_t>(s.integer("modbus_timeout_ms", 10, 60000, DEFAULT_MODBUS_TIMEOUT_MS));
        cfg.response_timeout_ms = static_cast<uint32_t>(s.integer("response_timeout_ms", 10, 60000, DEFAULT_RESPONSE_TIMEOUT_MS));
        cfg.max_retries = static_cast<uint8_t>(s.integer("max_retries", 0, 20, DEFAULT_MAX_RETRIES));
        cfg.retry_delay_ms = static_cast<uint32_t>(s.integer("retry_delay_ms", 0, 60000, DEFAULT_RETRY_DELAY_MS));
    } else {
        cfg.poll_interval_sec = DEFAULT_POLL_INTERVAL_SEC;
        cfg.eeprom_check_interval_sec = DEFAULT_EEPROM_CHECK_INTERVAL_SEC;
        cfg.modbus_timeout_ms = DEFAULT_MODBUS_TIMEOUT_MS;
        cfg.response_timeout_ms = DEFAULT_RESPONSE_TIMEOUT_MS;
        cfg.max_retries = DEFAULT_MAX_RETRIES;
        cfg.retry_delay_ms = DEFAULT_RETRY_DELAY_MS;
    }

    static const std::set<int64_t> bauds = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    std::set<uint8_t> branch_ids;
    std::set<std::string> devices, names;
    size_t total_boards = 0;

    for (const Node& b : root.objects("branches")) {
        b.warn_unknown({"id", "device", "baud_rate", "data_bits", "parity", "stop_bits", "boards"});
        BranchConfig br;
        br.id = static_cast<uint8_t>(b.integer("id", 0, static_cast<int64_t>(MAX_RS485_BRANCHES) - 1));
        if (!branch_ids.insert(br.id).second) b.fail("id", "ветка " + std::to_string(br.id) + " описана дважды");
        br.device = b.string("device");
        if (!devices.insert(br.device).second) b.fail("device", br.device + " уже занят другой веткой");
        const int64_t baud = b.integer("baud_rate", 1200, 115200, DEFAULT_BAUD_RATE);
        if (!bauds.count(baud)) b.fail("baud_rate", std::to_string(baud) + " — нестандартная скорость");
        br.baud_rate = static_cast<uint32_t>(baud);
        br.data_bits = static_cast<uint8_t>(b.integer("data_bits", 7, 8, DEFAULT_DATA_BITS));
        const std::string parity = b.string("parity", std::string(1, DEFAULT_PARITY));
        if (parity != "N" && parity != "E" && parity != "O")
            b.fail("parity", "\"" + parity + "\" — допустимо N, E или O");
        br.parity = parity[0];
        br.stop_bits = static_cast<uint8_t>(b.integer("stop_bits", 1, 2, DEFAULT_STOP_BITS));

        std::set<uint8_t> addrs;
        for (const Node& bd : b.objects("boards")) {
            bd.warn_unknown({"address", "name", "enabled"});
            BoardConfig board;
            board.address = static_cast<uint8_t>(bd.integer("address", MODBUS_MIN_ADDRESS, MODBUS_MAX_ADDRESS));
            if (!addrs.insert(board.address).second)
                bd.fail("address", "адрес " + std::to_string(board.address) + " уже занят на этой ветке");
            if (board.address == MODBUS_FACTORY_ADDRESS)
                bd.warn("address", "247 — заводской адрес новой платы; назначьте рабочий адрес");
            board.name = bd.string("name");
            if (!names.insert(board.name).second) bd.fail("name", "имя " + board.name + " уже используется");
            board.enabled = bd.boolean("enabled", true);
            br.boards.push_back(board);
        }
        if (br.boards.size() > MAX_BOARDS_PER_BRANCH)
            b.fail("boards", std::to_string(br.boards.size()) + " плат — больше " + std::to_string(MAX_BOARDS_PER_BRANCH)
                                 + " на ветку");
        if (br.boards.empty()) b.warn("boards", "на ветке нет плат");
        total_boards += br.boards.size();
        cfg.branches.push_back(br);
    }
    if (cfg.branches.empty()) root.fail("branches", "список веток пуст");

    if (root.has("power_supply")) cfg.power_supply = parse_psu(root.object("power_supply"));

    LOG_INFO("Config " + source + ": веток RS-485: " + std::to_string(cfg.branches.size()) + ", плат: "
             + std::to_string(total_boards));
    return cfg;
}

SystemConfig ConfigLoader::load_system_config(const std::string& filename) {
    return parse_system_config(read_file(filename), filename);
}

// =============================================================================
// eeprom_reference.json
// =============================================================================

std::map<std::string, BoardConfig> ConfigLoader::parse_eeprom_reference(const std::string& text,
                                                                         const std::string& source) {
    const json j = parse_text(text, source);
    const Node root(j, "", source);
    root.warn_unknown({"description", "last_updated", "boards"});

    std::map<std::string, BoardConfig> out;
    for (const Node& b : root.objects("boards")) {
        b.warn_unknown({"name", "modbus_address", "branch_id", "thermometer_ids", "cpu_id"});
        BoardConfig ref;
        ref.name = b.string("name");
        ref.address = static_cast<uint8_t>(b.integer("modbus_address", MODBUS_MIN_ADDRESS, MODBUS_MAX_ADDRESS));
        b.integer("branch_id", 0, static_cast<int64_t>(MAX_RS485_BRANCHES) - 1, 0);  // проверка диапазона
        ref.enabled = true;

        for (const std::string& id : b.strings("thermometer_ids")) {
            if (id.size() != 16 || !is_hex(id))
                b.fail("thermometer_ids", "\"" + id + "\" — ID 1-Wire должен быть 16 шестнадцатеричных цифр");
            ref.thermometer_ids.push_back(upper(id));
        }
        if (ref.thermometer_ids.size() != NUM_THERMOMETERS)
            b.warn("thermometer_ids", std::to_string(ref.thermometer_ids.size()) + " ID вместо "
                                          + std::to_string(NUM_THERMOMETERS));
        ref.cpu_id = b.string("cpu_id");
        if (!is_hex(ref.cpu_id)) b.fail("cpu_id", "\"" + ref.cpu_id + "\" — ожидаются шестнадцатеричные цифры");
        ref.cpu_id = upper(ref.cpu_id);

        if (!out.emplace(ref.name, ref).second) b.fail("name", "плата " + ref.name + " описана дважды");
    }
    LOG_INFO("Config " + source + ": эталонов EEPROM: " + std::to_string(out.size()));
    return out;
}

std::map<std::string, BoardConfig> ConfigLoader::load_eeprom_reference(const std::string& filename) {
    return parse_eeprom_reference(read_file(filename), filename);
}

// =============================================================================
// power_supply.json
// =============================================================================

PSUConfig ConfigLoader::parse_power_config(const std::string& text, const std::string& source) {
    const json j = parse_text(text, source);
    const Node root(j, "", source);
    root.warn_unknown({"description", "power_supply"});
    PSUConfig cfg = parse_psu(root.object("power_supply"));
    LOG_INFO("Config " + source + ": блок питания " + cfg.host + ":" + std::to_string(cfg.port) + ", каналов: "
             + std::to_string(cfg.channels.size()));
    return cfg;
}

std::string ConfigLoader::power_config_to_json(const PSUConfig& c) {
    json ps;
    ps["host"] = c.host;
    ps["port"] = c.port;
    ps["poll_interval_sec"] = c.poll_interval_sec;
    ps["connect_timeout_ms"] = c.connect_timeout_ms;
    ps["response_timeout_ms"] = c.response_timeout_ms;
    ps["max_retries"] = c.max_retries;
    ps["channels"] = json::array();
    for (const auto& ch : c.channels) {
        const char* role = ch.role == PSUChannelRole::CPU_POWER ? "CPU_POWER"
                         : ch.role == PSUChannelRole::SIPM_HV   ? "SIPM_HV" : "UNUSED";
        ps["channels"].push_back({{"channel", ch.channel}, {"role", role}, {"name", ch.name},
                                  {"target_voltage_v", ch.target_voltage_v}, {"current_limit_a", ch.current_limit_a},
                                  {"ovp_limit_v", ch.ovp_limit_v}, {"ocp_limit_a", ch.ocp_limit_a},
                                  {"ramp_step_v", ch.ramp_step_v}, {"ramp_interval_ms", ch.ramp_interval_ms},
                                  {"require_interlock", ch.require_interlock}});
    }
    return json{{"power_supply", ps}}.dump(2);
}

PSUConfig ConfigLoader::load_power_config(const std::string& filename) {
    return parse_power_config(read_file(filename), filename);
}
