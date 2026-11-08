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

BoardData board_with(uint16_t counter, uint16_t analog_mask) {
    BoardData d;
    d.data_valid = true;
    d.update_counter = counter;
    for (size_t ch = 0; ch < constants::NUM_SIPM_CHANNELS; ++ch)
        d.sipm_channels[ch].temperature =
            ((analog_mask >> constants::analog_board_of_channel(ch)) & 1) ? 2000 : collector::TEMPERATURE_NO_SENSOR;
    return d;
}

void test_collector() {
    std::printf("=== 1. DataCollector ===\n");
    // Плата 17 без периферии
    BoardData p17 = board_with(100, 0);
    for (auto& v : p17.dac_settings) v = 0;
    p17.dac_settings[0] = 1000;  // уставка есть, обратное чтение 0 — не ошибка без аналоговой платы
    auto h = collector::evaluate(p17, nullptr, 0);
    check(h.analog_boards() == 0, "без аналоговых плат: 0 подключено");
    check(h.dac_mismatches.empty(), "без аналоговых плат DAC не сверяется");
    check(h.ok(), "без аналоговых плат проблем нет");

    BoardData a = board_with(100, 0b001);
    a.dac_settings[5] = 300;   // плата 0 подключена, обратное чтение 0 — расхождение
    a.dac_settings[40] = 300;  // плата 1 не подключена — не сверяем
    h = collector::evaluate(a, nullptr, 0);
    check(h.analog_present[0] && !h.analog_present[1], "подключена только аналоговая плата 0");
    check(h.dac_mismatches.size() == 1 && h.dac_mismatches[0] == 5, "расхождение DAC только по ch5 (плата 0)");
    check(h.temperature_valid[0] && !h.temperature_valid[4], "температуры: ch0 валидна, ch4 — нет датчика");

    BoardData prev = board_with(65530, 0b001), now = board_with(4, 0b001);
    h = collector::evaluate(now, &prev, 2000);  // +10 циклов через переполнение за 2 с
    check(h.fresh && !h.rebooted, "переполнение счётчика 65530 -> 4 за 2 с — не перезагрузка");
    prev = board_with(30000, 0b001);
    now = board_with(500, 0b001);
    h = collector::evaluate(now, &prev, 300000);
    check(h.rebooted, "30000 -> 500 за 5 мин — перезагрузка");
    prev = board_with(500, 0b001);
    now = board_with(500, 0b001);
    h = collector::evaluate(now, &prev, 1000);
    check(!h.fresh && !h.ok(), "счётчик не изменился — данные не обновляются");

    BoardData t = board_with(1, 0b011);
    t.hv_status = static_cast<uint16_t>(0b110 << 12);  // trip на платах 1 и 2, плата 2 не подключена
    h = collector::evaluate(t, nullptr, 0);
    check(h.tripped_boards == 0b010, "trip учитывается только на подключённых платах (маска 0x2)");
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
    PeriodicPoller poller(cfg, reg);
    poller.set_poll_interval(0.5);
    poller.start();

    check(wait_for([&] { return status(reg, 1) == BoardStatus::ONLINE && status(reg, 2) == BoardStatus::ONLINE
                                && status(reg, 4) == BoardStatus::OFFLINE; }, 6),
          "1 и 2 online, 4 (нет на шине) offline");
    check(status(reg, 3) == BoardStatus::DISABLED, "3 (enabled: false) не опрашивается");
    auto s1 = reg.get(0, 1);
    check(s1 && s1->health.analog_boards() == 1 && s1->health.ok(), "плата 1: одна аналоговая плата, проблем нет");
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
    test_collector();
    if (argc >= 3) test_poller(argv[1], argv[2]);
    else std::printf("\n(опрос не проверялся: нужны аргументы <device> <control-file>)\n");
    std::printf("\n=== Итог: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
