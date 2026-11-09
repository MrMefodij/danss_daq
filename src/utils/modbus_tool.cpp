// modbus_tool — чтение, запись и обслуживание одной процессорной платы (карта регистров v2).
//
//   modbus_tool [-b 38400] [-t 500] <device> <address> <команда> [аргументы]
//
// Команды (только чтение):
//   info                       состояние, настройки, ID, сверка DAC
//   read input|holding <start> [count]   сырые регистры
//   watch [сек]                счётчик обновлений: период цикла платы, свежесть данных
//   dac                        DAC: уставка (holding 0-95) против применённого (input 150-245)
//   sp-dump [backup.json]      scratchpad: контрольная сумма, поля; с файлом — сохранить копию
// Команды (запись):
//   write <reg> <v>[,v...]     holding-регистр(ы); значения десятичные или 0x...
//   dac-set <канал|all> <v>    записать DAC и проверить обратным чтением после цикла платы
//   hv-enable <маска>          holding 96, бит на канал
//   sp-save                    стереть и сохранить scratchpad (103 -> 105), проверить
//   sp-load                    загрузить рабочие регистры из scratchpad (101)
//   sp-erase                   только стереть scratchpad (103) — первый шаг смены адреса
//   restore <backup.json>      вернуть DAC, HV enable, gain/trip, задержку из копии и сохранить
//   set-address <новый>        назначить адрес плате на заводском 247
//
// Перед sp-erase / set-address сделайте sp-dump backup.json: стирание уничтожает
// сохранённые в плате уставки.

#include "common/constants.h"
#include "common/logger.h"
#include "modbus/board_client.h"
#include "modbus/modbus_rtu.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace danss;
using namespace danss::constants;
using json = nlohmann::json;

namespace {

void usage() {
    std::printf(
        "Usage: modbus_tool [-b baud] [-t timeout_ms] <device> <address> <command> [args]\n\n"
        "Read:   info | read input|holding <start> [count] | watch [sec] | dac | sp-dump [backup.json]\n"
        "Write:  write <reg> <v>[,v...] | dac-set <ch|all> <v> | hv-enable <mask>\n"
        "        sp-save | sp-load | sp-erase | restore <backup.json> | set-address <new>\n\n"
        "Example: modbus_tool /dev/cu.usbserial-1410 1 info\n");
}

long parse_num(const std::string& s) {
    size_t pos = 0;
    long v = std::stol(s, &pos, 0);  // 0x.. — шестнадцатеричное
    if (pos != s.size()) throw std::invalid_argument("not a number: " + s);
    return v;
}

uint16_t parse_u16(const std::string& s) {
    long v = parse_num(s);
    if (v < 0 || v > 0xFFFF) throw std::invalid_argument("value out of 0..65535: " + s);
    return static_cast<uint16_t>(v);
}

std::string bits(uint16_t v, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i)
        if ((v >> i) & 1u) out += (out.empty() ? "" : ",") + std::to_string(i);
    return out.empty() ? "-" : out;
}

std::string hex4(uint16_t v) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%04X", v);
    return b;
}

int die(const BoardClient& b) {
    std::fprintf(stderr, "ERROR: %s\n", b.last_error().c_str());
    return 1;
}

// ---------------------------------------------------------------------------

int cmd_info(BoardClient& b) {
    BoardData d;
    if (!b.read_status(d)) return die(b);
    std::printf("Board %u\n", b.address());
    std::printf("  update counter   %u\n", d.update_counter);
    std::printf("  HV status        %s  enabled ch: %s  trip (analog boards): %s\n", hex4(d.hv_status).c_str(),
                bits(hv_enabled_channels(d.hv_status), NUM_SIPM_CHANNELS).c_str(),
                bits(tripped_analog_boards(d.hv_status), NUM_ANALOG_BOARDS).c_str());
    std::printf("  gain/trip status %s  trip enable: %s  gain: %s\n", hex4(d.gain_trip_status).c_str(),
                bits(trip_enable_bits(d.gain_trip_status), NUM_ANALOG_BOARDS).c_str(),
                bits(gain_bits(d.gain_trip_status), NUM_ANALOG_BOARDS).c_str());
    std::printf("  CPU temperature  %u (raw)\n", d.cpu_temperature);

    bool ok = true;
    if (b.read_ids(d)) {
        std::printf("  CPU ID           %s   CPU temp offset %u\n", d.cpu_id.c_str(), d.cpu_temp_offset);
    } else {
        std::printf("  IDs: %s\n", b.last_error().c_str());
        ok = false;
    }
    if (b.read_settings(d)) {
        std::printf("  HV enable (96)   %s  ch: %s\n", hex4(d.hv_enable).c_str(), bits(d.hv_enable, NUM_SIPM_CHANNELS).c_str());
        std::printf("  gain/trip en (97) %s\n", hex4(d.gain_trip_enable).c_str());
        std::printf("  CSR status (98)  %u\n", d.global_csr);
        std::printf("  cycle delay (99) %u\n", d.cycle_delay);
        std::printf("  address (100)    %u\n", d.device_address);
    } else {
        std::printf("  settings: %s\n", b.last_error().c_str());
        ok = false;
    }
    if (b.read_dac_readback(d)) {
        auto mm = d.dac_mismatches();
        std::printf("  DAC readback     %s\n",
                    mm.empty() ? "all 96 equal to setpoints" : (std::to_string(mm.size()) + " channels differ (see 'dac')").c_str());
        // Аналоговая плата видна, если не все 32 обратных чтения её DAC = 0 (письмо Игоря 09.11.2026)
        std::string seen;
        for (size_t a = 0; a < NUM_ANALOG_BOARDS; ++a) {
            bool any = false;
            for (size_t i = a * 32; i < (a + 1) * 32; ++i) any = any || d.dac_readback[i] != 0;
            seen += any ? std::to_string(a) + " " : std::string("- ");
        }
        std::printf("  analog boards    %s (seen = some DAC readback != 0)\n", seen.c_str());
    } else {
        std::printf("  DAC readback: %s\n", b.last_error().c_str());
        ok = false;
    }

    std::printf("\n  %3s %5s %8s %7s %6s %7s %4s   %s\n", "ch", "abrd", "I raw", "I uA", "T raw", "T C", "HV",
                "thermometer ID (0 = no counter)");
    for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch) {
        const auto& c = d.sipm_channels[ch];
        char t[16];
        if (c.temperature == SIPM_TEMP_NO_ACCESS) std::snprintf(t, sizeof(t), "n/a");
        else std::snprintf(t, sizeof(t), "%.2f", sipm_temperature_c(c.temperature));
        std::printf("  %3zu %5zu %8u %7.3f %6u %7s %4s   %s\n", ch, analog_board_of_channel(ch), c.leakage_current,
                    sipm_current_ua(c.leakage_current), c.temperature, t, hv_channel_enabled(d.hv_status, ch) ? "on" : "off",
                    c.thermometer_id.c_str());
    }
    return ok ? 0 : 1;
}

int cmd_read(ModbusRTU& bus, uint8_t addr, const std::vector<std::string>& a) {
    if (a.size() < 2) { usage(); return 2; }
    const bool input = a[0] == "input";
    if (!input && a[0] != "holding") { usage(); return 2; }
    const uint16_t start = parse_u16(a[1]);
    const uint16_t count = a.size() > 2 ? parse_u16(a[2]) : 1;
    std::vector<uint16_t> v;
    bool ok = input ? bus.read_input_registers(addr, start, count, v) : bus.read_holding_registers(addr, start, count, v);
    if (!ok) {
        std::fprintf(stderr, "ERROR: %s\n", bus.get_last_error().c_str());
        return 1;
    }
    for (size_t i = 0; i < v.size(); ++i)
        std::printf("%s %4zu = %5u  %s\n", input ? "in " : "hld", start + i, v[i], hex4(v[i]).c_str());
    return 0;
}

int cmd_watch(BoardClient& b, double seconds) {
    std::printf("Watching update counter of board %u for %.0f s...\n", b.address(), seconds);
    const auto t0 = std::chrono::steady_clock::now();
    uint16_t last = 0;
    bool have = false;
    int changes = 0, failures = 0;
    double first_change = 0, last_change = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        uint16_t c = 0;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!b.read_update_counter(c)) {
            ++failures;
        } else if (!have) {
            have = true;
            last = c;
            std::printf("  %6.2f s  counter %u\n", t, c);
        } else if (c != last) {
            if (changes == 0) first_change = t;
            last_change = t;
            ++changes;
            std::printf("  %6.2f s  counter %u (+%u)\n", t, c, static_cast<uint16_t>(c - last));
            last = c;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (changes >= 2)
        std::printf("Cycle period ≈ %.3f s (%d changes), read failures: %d\n", (last_change - first_change) / (changes - 1),
                    changes, failures);
    else
        std::printf("Counter changed %d times — %s, read failures: %d\n", changes,
                    changes == 0 ? "board update loop is NOT running (stale data)" : "period longer than the watch time",
                    failures);
    return changes == 0 ? 1 : 0;
}

int cmd_dac(BoardClient& b) {
    BoardData d;
    if (!b.read_settings(d) || !b.read_dac_readback(d)) return die(b);
    std::printf("DAC ch: setpoint / applied   (* = differ)\n");
    for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) {
        std::printf("  %2zu: %5u / %5u%s%s", i, d.dac_settings[i], d.dac_readback[i],
                    d.dac_settings[i] != d.dac_readback[i] ? "*" : " ", (i % 4 == 3) ? "\n" : "   ");
    }
    auto mm = d.dac_mismatches();
    std::printf("%zu channel(s) differ\n", mm.size());
    return mm.empty() ? 0 : 1;
}

int cmd_dac_set(BoardClient& b, const std::vector<std::string>& a) {
    if (a.size() < 2) { usage(); return 2; }
    BoardData d;
    if (!b.read_settings(d)) return die(b);
    std::array<uint16_t, NUM_DAC_CHANNELS> v = d.dac_settings;
    const uint16_t value = parse_u16(a[1]);
    if (a[0] == "all") v.fill(value);
    else {
        long ch = parse_num(a[0]);
        if (ch < 0 || ch >= static_cast<long>(NUM_DAC_CHANNELS)) throw std::invalid_argument("DAC channel 0-95");
        v[static_cast<size_t>(ch)] = value;
    }
    std::vector<size_t> mm;
    if (!b.write_dac_verified(v, mm)) {
        std::fprintf(stderr, "ERROR: %s\n", b.last_error().c_str());
        for (size_t i : mm) std::fprintf(stderr, "  ch %zu\n", i);
        return 1;
    }
    std::printf("DAC written and confirmed by readback after the board cycle (not saved to scratchpad: use sp-save)\n");
    return 0;
}

json backup_json(uint8_t addr, const BoardData& d, const ScratchpadImage& sp) {
    json j;
    j["address"] = addr;
    j["dac"] = std::vector<uint16_t>(d.dac_settings.begin(), d.dac_settings.end());
    j["hv_enable"] = d.hv_enable;
    j["gain_trip_enable"] = d.gain_trip_enable;
    j["cycle_delay"] = d.cycle_delay;
    j["cpu_id"] = d.cpu_id;
    j["scratchpad"] = std::vector<uint16_t>(sp.words.begin(), sp.words.end());
    j["scratchpad_checksum_ok"] = sp.checksum_ok();
    return j;
}

int cmd_sp_dump(BoardClient& b, const std::vector<std::string>& a) {
    ScratchpadImage sp;
    if (!b.read_scratchpad(sp)) return die(b);
    std::printf("Scratchpad of board %u:\n", b.address());
    std::printf("  checksum         0x%04X  %s\n", sp.sum(), sp.checksum_ok() ? "OK (0xAA55)" : "BAD (expected 0xAA55)");
    std::printf("  address word     %u\n", sp.modbus_address());
    std::printf("  HV enable word   %s\n", hex4(sp.hv_enable()).c_str());
    std::printf("  cycle delay word %u\n", sp.cycle_delay());
    BoardData d;
    if (b.read_settings(d)) {
        size_t diff = 0;
        for (size_t i = 0; i < NUM_DAC_CHANNELS; ++i) diff += sp.dac(i) != d.dac_settings[i];
        std::printf("  DAC words        %zu of 96 differ from current setpoints\n", diff);
    }
    if (!a.empty()) {
        b.read_ids(d);
        std::ofstream f(a[0]);
        if (!f) {
            std::fprintf(stderr, "ERROR: cannot write %s\n", a[0].c_str());
            return 1;
        }
        f << backup_json(b.address(), d, sp).dump(2) << "\n";
        std::printf("Backup written to %s (current registers + full scratchpad)\n", a[0].c_str());
    }
    return sp.checksum_ok() ? 0 : 1;
}

int cmd_restore(BoardClient& b, const std::string& file) {
    std::ifstream f(file);
    if (!f) {
        std::fprintf(stderr, "ERROR: cannot read %s\n", file.c_str());
        return 1;
    }
    json j = json::parse(f);
    auto dac = j.at("dac").get<std::vector<uint16_t>>();
    if (dac.size() != NUM_DAC_CHANNELS) throw std::invalid_argument("backup: dac must have 96 values");
    std::array<uint16_t, NUM_DAC_CHANNELS> v{};
    std::copy(dac.begin(), dac.end(), v.begin());
    std::vector<size_t> mm;
    if (!b.write_dac_verified(v, mm)) return die(b);
    if (!b.write_hv_enable(j.at("hv_enable").get<uint16_t>()) ||
        !b.write_gain_trip_enable(j.at("gain_trip_enable").get<uint16_t>()) ||
        !b.write_cycle_delay(j.at("cycle_delay").get<uint16_t>()))
        return die(b);
    std::string report;
    if (!b.save_to_scratchpad(report)) return die(b);
    std::printf("Restored from %s and %s\n", file.c_str(), report.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    uint32_t baud = DEFAULT_BAUD_RATE, timeout = 500;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-b" && i + 1 < argc) baud = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (s == "-t" && i + 1 < argc) timeout = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (s == "-h" || s == "--help") { usage(); return 0; }
        else pos.push_back(s);
    }
    if (pos.size() < 3) { usage(); return 2; }

    Logger::instance().set_level(LogLevel::WARNING);
    try {
        const std::string device = pos[0];
        const long a = parse_num(pos[1]);
        if (a < MODBUS_MIN_ADDRESS || a > MODBUS_MAX_ADDRESS) throw std::invalid_argument("address 1-247");
        const uint8_t addr = static_cast<uint8_t>(a);
        const std::string cmd = pos[2];
        const std::vector<std::string> args(pos.begin() + 3, pos.end());

        ModbusRTU bus(device, baud);
        bus.set_timeout(timeout);
        if (!bus.open()) {
            std::fprintf(stderr, "ERROR: %s\n", bus.get_last_error().c_str());
            return 1;
        }
        BoardClient b(bus, addr);

        if (cmd == "info") return cmd_info(b);
        if (cmd == "read") return cmd_read(bus, addr, args);
        if (cmd == "watch") return cmd_watch(b, args.empty() ? 10.0 : std::stod(args[0]));
        if (cmd == "dac") return cmd_dac(b);
        if (cmd == "dac-set") return cmd_dac_set(b, args);
        if (cmd == "sp-dump") return cmd_sp_dump(b, args);
        if (cmd == "write") {
            if (args.size() < 2) { usage(); return 2; }
            const uint16_t reg = parse_u16(args[0]);
            std::vector<uint16_t> vals;
            std::stringstream ss(args[1]);
            for (std::string t; std::getline(ss, t, ',');) vals.push_back(parse_u16(t));
            const bool ok = vals.size() == 1 ? bus.write_single_register(addr, reg, vals[0])
                                             : bus.write_multiple_registers(addr, reg, vals);
            if (!ok) { std::fprintf(stderr, "ERROR: %s\n", bus.get_last_error().c_str()); return 1; }
            std::printf("written %zu register(s) at holding %u\n", vals.size(), reg);
            return 0;
        }
        if (cmd == "hv-enable") {
            if (args.empty()) { usage(); return 2; }
            if (!b.write_hv_enable(parse_u16(args[0]))) return die(b);
            std::printf("HV enable mask written (holding 96)\n");
            return 0;
        }
        if (cmd == "sp-save") {
            std::string report;
            if (!b.save_to_scratchpad(report)) return die(b);
            std::printf("%s\n", report.c_str());
            return 0;
        }
        if (cmd == "sp-load") {
            if (!b.load_from_scratchpad()) return die(b);
            std::printf("registers loaded from scratchpad (101)\n");
            return 0;
        }
        if (cmd == "sp-erase") {
            if (!b.erase_scratchpad()) return die(b);
            std::printf("scratchpad erased (103). After a power cycle the board comes up at address 247.\n");
            return 0;
        }
        if (cmd == "restore") {
            if (args.empty()) { usage(); return 2; }
            return cmd_restore(b, args[0]);
        }
        if (cmd == "set-address") {
            if (args.empty()) { usage(); return 2; }
            const long n = parse_num(args[0]);
            if (n < 1 || n > 246) throw std::invalid_argument("new address 1-246");
            std::string report;
            if (!b.assign_address(static_cast<uint8_t>(n), report)) return die(b);
            std::printf("%s\n", report.c_str());
            return 0;
        }
        std::fprintf(stderr, "unknown command: %s\n\n", cmd.c_str());
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 2;
    }
}
