// Тест опроса плат: DataCollector (без железа) + BoardRegistry/PeriodicPoller против
// имитатора fake_modbus_slave.py с несколькими платами и сценариями сбоев.
//
//   python3 tests/fake_modbus_slave.py --address 1,2 --analog 1 --period 0.2 --control /tmp/ctl.txt &
//   ./test_poller /dev/pts/N /tmp/ctl.txt
//
// Без аргументов — только проверки DataCollector.

#include "common/logger.h"
#include "rs485/board_registry.h"
#include "slowcontrol/data_collector.h"
#include "slowcontrol/periodic_poller.h"
#include "slowcontrol/exclusions_file.h"
#include "common/config_loader.h"

#include <cmath>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

using namespace danss;
using clk = std::chrono::steady_clock;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    (cond ? g_pass : g_fail)++;
}

/// Плата: analog_mask — какие аналоговые платы видны (DAC readback ≠ 0), counter_mask — какие счётчики (ID ≠ 0)
BoardData board_with(uint16_t counter, uint16_t analog_mask, uint16_t counter_mask = 0) {
    BoardData d;
    d.data_valid = true;
    d.update_counter = counter;
    for (size_t i = 0; i < constants::NUM_DAC_CHANNELS; ++i) {
        d.dac_settings[i] = static_cast<uint16_t>(100 + i);
        d.dac_readback[i] = ((analog_mask >> collector::analog_board_of_dac(i)) & 1) ? d.dac_settings[i] : 0;
    }
    for (size_t ch = 0; ch < constants::NUM_SIPM_CHANNELS; ++ch) {
        const bool c = (counter_mask >> ch) & 1;
        d.sipm_channels[ch].thermometer_id = c ? "28FF641E83160550" : "0000000000000000";
        d.sipm_channels[ch].temperature = c ? 400 : collector::TEMPERATURE_NO_SENSOR;
    }
    return d;
}

void test_units() {
    std::printf("=== 0. Единицы (письмо Игоря 09.11.2026) ===\n");
    check(std::abs(constants::sipm_current_ua(1024) - 6.6) < 1e-9, "ток: 1024 -> 6.6 мкА");
    check(std::abs(constants::sipm_current_ua(345) - 2.2236328125) < 1e-9, "ток: 345 -> 2.224 мкА");
    check(constants::sipm_temperature_c(400) == 25.0, "температура: 400 -> 25 °C");
    check(constants::sipm_temperature_c(0xFFF0) == -1.0, "температура: 0xFFF0 -> -1 °C (знаковая)");
    check(collector::mask_to_list(0b1110110001, 12) == "0, 4, 5, 7-9", "mask_to_list");
}

void test_collector() {
    std::printf("\n=== 1. DataCollector ===\n");
    const uint16_t ALL_A = constants::ALL_ANALOG_BOARDS_MASK, ALL_C = constants::ALL_COUNTERS_MASK;

    // Полная плата
    auto h = collector::evaluate(board_with(100, ALL_A, ALL_C), nullptr, 0);
    check(h.ok() && !h.missing(), "всё на месте — проблем нет");
    check(h.analog_boards() == 3 && h.counters() == 12, "видно 3 аналоговые платы и 12 счётчиков");

    // Плата 17: ничего не подключено, ничего не исключено — всё отсутствует
    BoardData p17 = board_with(100, 0, 0);
    h = collector::evaluate(p17, nullptr, 0);
    check(h.missing_analog == ALL_A && h.missing_counters == ALL_C && !h.ok(),
          "без периферии и без исключений: отсутствуют 3 платы и 12 счётчиков");
    check(h.dac_mismatches.empty(), "DAC неподключённых плат не сверяется (readback 0 — не расхождение)");
    // ...а всё исключено вручную
    h = collector::evaluate(p17, nullptr, 0, {ALL_A, ALL_C});
    check(h.ok() && !h.missing(), "плата 17, всё исключено вручную: проблем нет");

    // Частично: аналоговая плата 2 и счётчики 8–11 сняты, исключены только счётчики
    BoardData part = board_with(100, 0b011, 0x0FF);
    h = collector::evaluate(part, nullptr, 0, {0, 0xF00});
    check(h.missing_analog == 0b100 && h.missing_counters == 0, "не исключённая аналоговая плата 2 — отсутствует");
    check(h.problems.size() == 1, "одна проблема: " + (h.problems.empty() ? std::string("-") : h.problems[0]));

    // Счётчик есть, а температура не читается
    BoardData terr = board_with(100, ALL_A, ALL_C);
    terr.sipm_channels[3].temperature = collector::TEMPERATURE_NO_SENSOR;
    h = collector::evaluate(terr, nullptr, 0);
    check(h.temperature_errors == (1u << 3), "ID есть, температура 0x8001 — ошибка температуры на канале 3");

    // DAC: расхождение на подключённой плате 0, исключённая плата 1 не сверяется
    BoardData dac = board_with(100, ALL_A, ALL_C);
    dac.dac_readback[5] = 7;
    dac.dac_readback[40] = 7;
    h = collector::evaluate(dac, nullptr, 0, {0b010, 0});
    check(h.dac_mismatches.size() == 1 && h.dac_mismatches[0] == 5, "расхождение DAC только ch5 (плата 1 исключена)");

    // Счётчик циклов
    BoardData prev = board_with(65530, ALL_A, ALL_C), now = board_with(4, ALL_A, ALL_C);
    h = collector::evaluate(now, &prev, 2000);
    check(h.fresh && !h.rebooted, "переполнение счётчика 65530 -> 4 за 2 с — не перезагрузка");
    prev = board_with(30000, ALL_A, ALL_C);
    now = board_with(500, ALL_A, ALL_C);
    h = collector::evaluate(now, &prev, 300000);
    check(h.rebooted, "30000 -> 500 за 5 мин — перезагрузка");
    prev = board_with(500, ALL_A, ALL_C);
    now = board_with(500, ALL_A, ALL_C);
    h = collector::evaluate(now, &prev, 1000);
    check(!h.fresh && !h.ok(), "счётчик не изменился — данные не обновляются");

    // Trip: учитывается на подключённых и не исключённых платах
    BoardData t = board_with(1, 0b011, ALL_C);
    t.hv_status = static_cast<uint16_t>(0b111 << 12);
    h = collector::evaluate(t, nullptr, 0, {0b101, 0});
    check(h.tripped_boards == 0b010, "trip: плата 0 исключена, 2 не подключена — остаётся 0x2");
}

void test_exclusions_file() {
    std::printf("\n=== 1a. Файл исключений ===\n");
    ExclusionMap m;
    m["BENCH-17"] = {{0b111, 0xFFF}, "стол", "2026-11-09 10:00:00"};
    m["EMPTY"] = {};
    const std::string text = exclusions_to_json(m);
    auto back = parse_exclusions(text, "test");
    check(back.size() == 1 && back.count("BENCH-17"), "пустые исключения в файл не пишутся");
    check(back["BENCH-17"].masks.analog_boards == 0b111 && back["BENCH-17"].masks.counters == 0xFFF
              && back["BENCH-17"].reason == "стол",
          "запись/чтение сохраняют маски и причину");
    bool threw = false;
    try {
        parse_exclusions(R"({"boards":{"X":{"excluded_counters":[12]}}})", "test");
    } catch (const ConfigError&) { threw = true; }
    check(threw, "канал 12 — ошибка файла");
    const std::string f = "/tmp/test_exclusions_" + std::to_string(::getpid()) + ".json";
    save_exclusions(f, m);
    bool exists = false;
    auto loaded = load_exclusions(f, exists);
    check(exists && loaded.size() == 1, "save/load через файл");
    std::remove(f.c_str());
    loaded = load_exclusions(f, exists);
    check(!exists && loaded.empty(), "файла нет — пусто, exists = false");
}

void ctl(const std::string& file, const std::string& line) {
    std::ofstream(file, std::ios::app) << line << "\n";
}

bool wait_for(const std::function<bool()>& cond, double timeout_s) {
    auto end = clk::now() + std::chrono::milliseconds(static_cast<int>(timeout_s * 1000));
    while (clk::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

BoardStatus status(const BoardRegistry& r, uint8_t addr) {
    auto s = r.get(0, addr);
    return s ? s->status : BoardStatus::UNKNOWN;
}

void test_poller(const std::string& device, const std::string& control) {
    std::printf("\n=== 2. PeriodicPoller + BoardRegistry (имитатор: платы 1, 2; аналоговая плата 0) ===\n");
    SystemConfig cfg;
    cfg.poll_interval_sec = 1;
    cfg.response_timeout_ms = 150;
    cfg.max_retries = 1;
    cfg.retry_delay_ms = 20;
    BranchConfig br;
    br.id = 0;
    br.device = device;
    br.baud_rate = 38400;
    br.data_bits = 8;
    br.parity = 'N';
    br.stop_bits = 1;
    auto board = [](uint8_t a, const char* n, bool en) { BoardConfig b; b.address = a; b.name = n; b.enabled = en; return b; };
    br.boards = {board(1, "B1", true), board(2, "B2", true), board(3, "B3-off", false), board(4, "B4-absent", true)};
    cfg.branches = {br};

    BoardRegistry reg(cfg);
    // На «шине» у плат 1 и 2 только аналоговая плата 0 — платы 1 и 2 исключены вручную
    reg.set_exclusions(0, 1, {0b110, 0});
    reg.set_exclusions(0, 2, {0b110, 0});
    PeriodicPoller poller(cfg, reg);
    poller.set_poll_interval(0.5);
    poller.start();

    check(wait_for([&] { return status(reg, 1) == BoardStatus::ONLINE && status(reg, 2) == BoardStatus::ONLINE
                                && status(reg, 4) == BoardStatus::OFFLINE; }, 6),
          "1 и 2 online, 4 (нет на шине) offline");
    check(status(reg, 3) == BoardStatus::DISABLED, "3 (enabled: false) не опрашивается");
    auto s1 = reg.get(0, 1);
    check(s1 && s1->health.analog_boards() == 1 && s1->health.counters() == 12 && s1->health.ok(),
          "плата 1: одна аналоговая плата (2 исключены), 12 счётчиков, проблем нет");
    check(s1 && s1->data.sipm_channels[11].temperature == 0xFFF0
              && constants::sipm_temperature_c(s1->data.sipm_channels[11].temperature) == -1.0,
          "температура канала 11 = -1 °C (знаковая)");
    check(!reg.all_enabled_healthy(), "не все платы здоровы (4 offline)");

    ctl(control, "2 freeze");
    check(wait_for([&] { return status(reg, 2) == BoardStatus::STALE; }, 5), "плата 2: цикл встал -> stale");
    ctl(control, "2 unfreeze");
    check(wait_for([&] { return status(reg, 2) == BoardStatus::ONLINE; }, 5), "плата 2: цикл пошёл -> online");

    const uint64_t r0 = reg.get(0, 1)->reboots;
    ctl(control, "1 reboot");
    check(wait_for([&] { return reg.get(0, 1)->reboots > r0; }, 5), "плата 1: перезагрузка замечена");
    check(wait_for([&] { return status(reg, 1) == BoardStatus::ONLINE && reg.get(0, 1)->health.ok(); }, 5),
          "плата 1 после перезагрузки снова online без проблем");

    ctl(control, "1 trip 3");  // trip на платах 0 и 1; подключена только 0
    check(wait_for([&] { return reg.get(0, 1)->health.tripped_boards == 1; }, 5), "trip на подключённой плате 0 виден");
    check(reg.summary().tripped == 1, "summary: 1 плата с trip");
    ctl(control, "1 trip 0");
    check(wait_for([&] { return reg.get(0, 1)->health.tripped_boards == 0; }, 5), "trip снят");

    ctl(control, "2 analog 3");
    check(wait_for([&] { return reg.get(0, 2)->health.analog_boards() == 2; }, 5), "плата 2: подключили вторую аналоговую плату");

    ctl(control, "1 counters 0xFF0");  // сняли счётчики 0–3
    check(wait_for([&] { return reg.get(0, 1)->health.missing_counters == 0xF; }, 5), "плата 1: счётчики 0–3 не видны");
    check(!reg.get(0, 1)->health.ok() && reg.summary().with_missing >= 1, "это проблема платы (не исключены)");
    reg.set_exclusions(0, 1, {0b110, 0xF});
    poller.poll_now();
    check(wait_for([&] { return reg.get(0, 1)->health.ok(); }, 5), "оператор исключил счётчики 0–3 — проблем нет");
    reg.set_exclusions(0, 1, {0b110, 0});
    ctl(control, "1 counters 0xFFF");
    check(wait_for([&] { return reg.get(0, 1)->health.counters() == 12 && reg.get(0, 1)->health.ok(); }, 5),
          "счётчики вернули, исключение снято — проблем нет");

    ctl(control, "2 silent");
    check(wait_for([&] { return status(reg, 2) == BoardStatus::OFFLINE; }, 6), "плата 2 перестала отвечать -> offline");
    check(!reg.get(0, 2)->last_error.empty(), "причина записана: " + reg.get(0, 2)->last_error);
    ctl(control, "2 talk");
    check(wait_for([&] { return status(reg, 2) == BoardStatus::ONLINE; }, 6), "плата 2 снова отвечает -> online");

    auto t0 = clk::now();
    poller.stop();
    check(std::chrono::duration<double>(clk::now() - t0).count() < 2.0, "stop() быстрый");
    auto st = poller.branch_stats(0);
    check(st.total_transactions > 0 && st.successful_reads > 0, "статистика ветки: " + std::to_string(st.successful_reads)
                                                                  + "/" + std::to_string(st.total_transactions) + " ответов");
}

} // namespace

int main(int argc, char** argv) {
    Logger::instance().set_level(constants::LogLevel::ERROR);
    test_units();
    test_collector();
    test_exclusions_file();
    if (argc >= 3) test_poller(argv[1], argv[2]);
    else std::printf("\n(опрос не проверялся: нужны аргументы <device> <control-file>)\n");
    std::printf("\n=== Итог: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
