// Тесты ConfigLoader: реальные файлы из config/ и набор заведомо ошибочных конфигураций.
//
//   ./test_config_loader [каталог с config/, по умолчанию .]
//
// Не требует MIDAS и железа. Сборка без CMake:
//   g++ -std=c++17 -Iinclude tests/test_config_loader.cpp src/utils/config_loader.cpp src/utils/logger.cpp -pthread

#include "common/config_loader.h"
#include "common/logger.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace danss;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    (cond ? g_pass : g_fail)++;
}

/// Ожидаем ConfigError, в тексте которого есть `fragment`
void expect_error(const std::string& what, const std::function<void()>& fn, const std::string& fragment) {
    try {
        fn();
        check(false, what + " — исключения не было");
    } catch (const ConfigError& e) {
        const bool ok = std::string(e.what()).find(fragment) != std::string::npos;
        check(ok, what);
        if (!ok) std::printf("         сообщение: %s\n         ждали фрагмент: %s\n", e.what(), fragment.c_str());
        else std::printf("         %s\n", e.what());
    }
}

// Минимальный корректный power_supply.json; `ch1` подставляется как описание канала 1
std::string psu_json(const std::string& ch1, const std::string& extra_root = "") {
    return R"({ "power_supply": { "host": "192.168.10.81", "channels": [
        { "channel": 3, "role": "CPU_POWER", "target_voltage_v": 6.0, "current_limit_a": 3.0,
          "ovp_limit_v": 6.5, "ocp_limit_a": 3.1 }, )" + ch1 + R"( ] } )" + extra_root + "}";
}

const std::string HV_OK = R"({ "channel": 1, "role": "SIPM_HV", "target_voltage_v": 55.0, "current_limit_a": 0.5,
                               "ovp_limit_v": 57.0, "ocp_limit_a": 0.7 })";

std::string hv_with(const std::string& key, const std::string& value) {
    std::string s = HV_OK;
    const auto p = s.find("\"" + key + "\"");
    if (p == std::string::npos) {  // добавить поле
        s.insert(s.rfind('}'), ", \"" + key + "\": " + value + " ");
        return s;
    }
    const auto colon = s.find(':', p);
    const auto end = s.find_first_of(",}", colon);
    return s.substr(0, colon + 1) + " " + value + s.substr(end);
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    Logger::instance().set_level(constants::LogLevel::ERROR);  // предупреждения не засоряют вывод

    std::printf("=== 1. Реальные файлы из config/ ===\n");
    try {
        auto sys = ConfigLoader::load_system_config(dir + "/config/board_map.json");
        check(!sys.branches.empty(), "board_map.json: есть ветки (" + std::to_string(sys.branches.size()) + ")");
        check(sys.poll_interval_sec == 300, "board_map.json: poll_interval_sec = 300");
        check(sys.branches[0].baud_rate == 38400 && sys.branches[0].parity == 'N', "board_map.json: 38400 8N1");
        check(!sys.branches[0].boards.empty() && sys.branches[0].boards[0].address == 1, "board_map.json: первая плата адрес 1");
    } catch (const ConfigError& e) {
        check(false, std::string("board_map.json: ") + e.what());
    }
    try {
        auto st = ConfigLoader::load_system_config(dir + "/config/board_map_stand.json");
        check(st.branches.size() == 1 && st.branches[0].device == "/dev/ttyACM3", "board_map_stand.json: одна ветка, /dev/ttyACM3");
        check(st.branches[0].boards.size() == 3 && !st.branches[0].boards[2].enabled, "board_map_stand.json: платы 1, 2 и выключенная 3");
    } catch (const ConfigError& e) {
        check(false, std::string("board_map_stand.json: ") + e.what());
    }
    try {
        auto ref = ConfigLoader::load_eeprom_reference(dir + "/config/eeprom_reference.json");
        check(!ref.empty(), "eeprom_reference.json: есть эталоны (" + std::to_string(ref.size()) + ")");
        auto it = ref.find("PMT-01-01");
        check(it != ref.end() && it->second.thermometer_ids.size() == 12, "eeprom_reference.json: PMT-01-01, 12 ID");
    } catch (const ConfigError& e) {
        check(false, std::string("eeprom_reference.json: ") + e.what());
    }
    try {
        auto psu = ConfigLoader::load_power_config(dir + "/config/power_supply.json");
        check(psu.port == 3000 && !psu.host.empty(), "power_supply.json: host/port");
        const PSUChannelConfig* hv = nullptr;
        for (const auto& c : psu.channels) if (c.channel == 1) hv = &c;
        check(hv && hv->role == PSUChannelRole::SIPM_HV && hv->require_interlock, "power_supply.json: ch1 SIPM_HV с интерлоком");
        check(hv && std::fabs(hv->ovp_limit_v - 57.0) < 1e-9, "power_supply.json: ch1 OVP 57 В");
    } catch (const ConfigError& e) {
        check(false, std::string("power_supply.json: ") + e.what());
    }

    std::printf("\n=== 2. Значения по умолчанию ===\n");
    {
        auto psu = ConfigLoader::parse_power_config(psu_json(HV_OK), "mem");
        check(psu.port == constants::DEFAULT_PSU_PORT, "port по умолчанию 3000");
        check(psu.channels[1].ramp_interval_ms == constants::DEFAULT_PSU_RAMP_INTERVAL_MS, "ramp_interval_ms по умолчанию");
        check(psu.channels[1].require_interlock, "SIPM_HV: require_interlock по умолчанию true");
        check(!psu.channels[0].require_interlock, "CPU_POWER: require_interlock по умолчанию false");
        check(psu.channels[1].name == "ch1", "name по умолчанию \"ch1\"");
    }

    std::printf("\n=== 3. Ошибки в power_supply.json — защита HV ===\n");
    auto psu = [](const std::string& ch1) { return [ch1] { ConfigLoader::parse_power_config(psu_json(ch1), "psu.json"); }; };
    expect_error("OVP выше предела прибора (61 В)", psu(hv_with("ovp_limit_v", "70")), "channels[1].ovp_limit_v: 70");
    expect_error("OVP ниже цели", psu(hv_with("ovp_limit_v", "55.2")), "должно быть не ниже target_voltage_v");
    expect_error("OCP ниже предела тока", psu(hv_with("ocp_limit_a", "0.3")), "не ниже current_limit_a");
    expect_error("OCP 3.2 А — прибор не примет", psu(hv_with("ocp_limit_a", "3.2")), "ocp_limit_a: 3.2");
    expect_error("HV без интерлока", psu(hv_with("require_interlock", "false")), "интерлок обязателен");
    expect_error("слишком крупный шаг разгона HV", psu(hv_with("ramp_step_v", "10")), "шаг разгона");
    expect_error("цель выше 60 В", psu(hv_with("target_voltage_v", "65")), "target_voltage_v: 65");
    expect_error("напряжение строкой", psu(hv_with("target_voltage_v", "\"55\"")), "ожидается число");
    expect_error("неизвестная роль", psu(hv_with("role", "\"HV\"")), "допустимо CPU_POWER, SIPM_HV или UNUSED");
    expect_error("канал описан дважды", psu(R"({ "channel": 3, "role": "UNUSED" })"), "описан дважды");
    expect_error("канал 4", psu(hv_with("channel", "4")), "channel: 4");
    expect_error("ch3 выше 6 В",
                 [] { ConfigLoader::parse_power_config(R"({ "power_supply": { "host": "h", "channels": [
                     { "channel": 3, "role": "CPU_POWER", "target_voltage_v": 6.5, "current_limit_a": 1,
                       "ovp_limit_v": 6.6, "ocp_limit_a": 1.1 } ] } })", "psu.json"); },
                 "target_voltage_v: 6.5");
    expect_error("нет host", [] { ConfigLoader::parse_power_config(R"({ "power_supply": { "channels": [] } })", "psu.json"); },
                 "power_supply.host: обязательное поле");

    std::printf("\n=== 4. Ошибки в board_map.json ===\n");
    auto sys = [](const std::string& t) { return [t] { ConfigLoader::parse_system_config(t, "map.json"); }; };
    const std::string b1 = R"({ "address": 1, "name": "A" })";
    auto map_with = [](const std::string& branch_extra, const std::string& boards) {
        return R"({ "branches": [ { "id": 0, "device": "/dev/ttyACM3")" + branch_extra + R"(, "boards": [ )" + boards
               + R"( ] } ] })";
    };
    check([&] { try { auto c = ConfigLoader::parse_system_config(map_with("", b1), "m"); return c.branches[0].boards.size() == 1; }
                catch (...) { return false; } }(),
          "минимальная карта (одна ветка, одна плата, без раздела system)");
    expect_error("повтор адреса на ветке", sys(map_with("", b1 + "," + R"({ "address": 1, "name": "B" })")), "уже занят на этой ветке");
    expect_error("повтор имени", sys(map_with("", b1 + "," + R"({ "address": 2, "name": "A" })")), "уже используется");
    expect_error("адрес 0", sys(map_with("", R"({ "address": 0, "name": "A" })")), "address: 0");
    expect_error("нестандартная скорость", sys(map_with(R"(, "baud_rate": 20000)", b1)), "нестандартная скорость");
    expect_error("чётность X", sys(map_with(R"(, "parity": "X")", b1)), "допустимо N, E или O");
    expect_error("битый JSON", sys("{ \"branches\": [ "), "некорректный JSON");
    expect_error("нет списка веток", sys("{ }"), "branches: обязательный список");
    expect_error("файл не найден", [] { ConfigLoader::load_system_config("/nonexistent/board_map.json"); }, "не удалось открыть");

    std::printf("\n=== 5. Ошибки в eeprom_reference.json ===\n");
    auto eep = [](const std::string& id, const std::string& cpu) {
        return [id, cpu] {
            ConfigLoader::parse_eeprom_reference(R"({ "boards": [ { "name": "A", "modbus_address": 1,
                "thermometer_ids": [")" + id + R"("], "cpu_id": ")" + cpu + R"(" } ] })", "eeprom.json");
        };
    };
    expect_error("ID термометра не 16 hex-цифр", eep("28FF641E831605", "ABC123"), "16 шестнадцатеричных цифр");
    expect_error("ID термометра с не-hex символом", eep("28FF641E8316055Z", "ABC123"), "16 шестнадцатеричных цифр");
    expect_error("CPU ID с не-hex символом", eep("28FF641E8316055A", "XYZ"), "cpu_id");
    {
        auto r = ConfigLoader::parse_eeprom_reference(R"({ "boards": [ { "name": "A", "modbus_address": 1,
            "thermometer_ids": ["28ff641e8316055a"], "cpu_id": "abc123" } ] })", "eeprom.json");
        check(r["A"].thermometer_ids[0] == "28FF641E8316055A" && r["A"].cpu_id == "ABC123",
              "ID приводятся к верхнему регистру (для сравнения с прочитанными из платы)");
    }

    std::printf("\n=== Итог: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
