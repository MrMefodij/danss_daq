// Тест PsuWorker против mock_owon_server.py (без MIDAS и без железа).
//
//   python3 tests/mock_owon_server.py 15000 &
//   ./test_psu_worker 127.0.0.1 15000
//
// Проверяет то, ради чего worker написан: команды не блокируют вызывающего,
// «выключить» прерывает разгон, снятие интерлока и аварийное выключение срабатывают
// во время разгона, новая цель применяется к включённому каналу.

#include "common/logger.h"
#include "power/psu_worker.h"

#include <chrono>
#include <cmath>
#include <cstdio>
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

PSUChannelConfig chan(uint8_t n, PSUChannelRole role, double v, bool interlock) {
    PSUChannelConfig c{};
    c.channel = n;
    c.role = role;
    c.name = "ch" + std::to_string(n);
    c.target_voltage_v = v;
    c.current_limit_a = role == PSUChannelRole::SIPM_HV ? 0.5 : 3.0;
    c.ovp_limit_v = v + 2.0;
    c.ocp_limit_a = c.current_limit_a + 0.1;
    c.ramp_step_v = 1.0;
    c.ramp_interval_ms = 100;  // 20 В — 2 с
    c.require_interlock = interlock;
    return c;
}

PSUConfig config(const std::string& host, uint16_t port, double hv) {
    PSUConfig c;
    c.host = host;
    c.port = port;
    c.poll_interval_sec = 1;
    c.connect_timeout_ms = 1000;
    c.response_timeout_ms = 1000;
    c.max_retries = 1;
    c.channels = {chan(3, PSUChannelRole::CPU_POWER, 6.0, false), chan(1, PSUChannelRole::SIPM_HV, hv, true)};
    return c;
}

const PSUChannelStatus* st(const PsuSnapshot& s, uint8_t ch) {
    for (const auto& c : s.channels)
        if (c.channel == ch) return &c;
    return nullptr;
}

/// Ждать условия по снимку не дольше timeout_s
bool wait_for(PsuWorker& w, const std::function<bool(const PsuSnapshot&)>& cond, double timeout_s) {
    auto end = clk::now() + std::chrono::milliseconds(static_cast<int>(timeout_s * 1000));
    while (clk::now() < end) {
        w.request_poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cond(w.snapshot())) return true;
    }
    return false;
}

bool ch_on(const PsuSnapshot& s, uint8_t ch) { auto c = st(s, ch); return c && c->data_valid && c->output_enabled; }
double ch_v(const PsuSnapshot& s, uint8_t ch) { auto c = st(s, ch); return c ? c->measured_voltage_v : -1; }

/// Время вызова — команды не должны блокировать (MIDAS-поток не должен ждать разгон)
double timed(const std::function<void()>& fn) {
    auto t0 = clk::now();
    fn();
    return std::chrono::duration<double>(clk::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = static_cast<uint16_t>(argc > 2 ? std::stoi(argv[2]) : 15000);
    Logger::instance().set_level(constants::LogLevel::ERROR);

    PsuWorker w(config(host, port, 20.0));
    w.start();

    std::printf("=== 1. Старт ===\n");
    auto s = w.snapshot();
    check(s.connected, "подключился");
    check(s.protections_ok, "пороги записаны и подтверждены");
    check(!s.desired_on[0] && !s.desired_on[2], "на старте ничего не приказано (выходы были выключены)");

    std::printf("\n=== 2. HV без интерлока ===\n");
    const uint64_t e0 = s.error_seq;
    w.request_enable(1, true);
    check(wait_for(w, [&](const PsuSnapshot& x) { return x.error_seq > e0; }, 3), "ошибка зафиксирована");
    s = w.snapshot();
    check(!s.desired_on[0] && !ch_on(s, 1), "команда отменена, ch1 выключен: " + s.last_error);

    std::printf("\n=== 3. Электроника, затем HV с интерлоком ===\n");
    w.request_enable(3, true);
    check(wait_for(w, [](const PsuSnapshot& x) { return ch_on(x, 3); }, 3), "ch3 включён");
    w.set_interlock(true);
    double t = timed([&] { w.request_enable(1, true); });
    check(t < 0.05, "request_enable() вернулся сразу (" + std::to_string(t * 1000).substr(0, 4) + " мс)");
    check(wait_for(w, [](const PsuSnapshot& x) { return !x.busy.empty(); }, 2), "worker занят разгоном: " + w.snapshot().busy);
    check(wait_for(w, [](const PsuSnapshot& x) { return ch_on(x, 1) && std::fabs(ch_v(x, 1) - 20) < 0.05 && x.busy.empty(); }, 6),
          "ch1 на 20 В");

    std::printf("\n=== 4. Новая цель для включённого канала ===\n");
    w.request_config(config(host, port, 10.0));
    check(wait_for(w, [](const PsuSnapshot& x) { return std::fabs(ch_v(x, 1) - 10) < 0.05 && x.busy.empty(); }, 6),
          "ch1 плавно перешёл на 10 В, остался включён");
    check(ch_on(w.snapshot(), 1), "ch1 включён");
    check(std::fabs(w.snapshot().config.channels[1].ovp_limit_v - 12.0) < 1e-9, "в снимке новая конфигурация (OVP 12 В)");

    std::printf("\n=== 5. «Выключить» посреди разгона ===\n");
    w.request_enable(1, false);
    check(wait_for(w, [](const PsuSnapshot& x) { return !ch_on(x, 1) && x.busy.empty(); }, 6), "ch1 опущен и выключен");
    w.request_config(config(host, port, 20.0));
    wait_for(w, [](const PsuSnapshot& x) { return x.config.channels[1].target_voltage_v == 20.0 && x.protections_ok; }, 3);
    w.request_enable(1, true);
    wait_for(w, [](const PsuSnapshot& x) { return ch_v(x, 1) > 4; }, 3);
    t = timed([&] { w.request_enable(1, false); });
    check(t < 0.05, "команда во время разгона не блокирует");
    check(wait_for(w, [](const PsuSnapshot& x) { return !ch_on(x, 1) && x.busy.empty(); }, 6), "разгон прерван, ch1 выключен");
    check(ch_on(w.snapshot(), 3), "ch3 (электроника) не тронут");

    std::printf("\n=== 6. Снятие интерлока посреди разгона ===\n");
    w.request_enable(1, true);
    wait_for(w, [](const PsuSnapshot& x) { return ch_v(x, 1) > 4; }, 3);
    t = timed([&] { w.set_interlock(false); });
    check(t < 0.05, "set_interlock(false) не блокирует (" + std::to_string(t * 1000).substr(0, 4) + " мс)");
    check(wait_for(w, [](const PsuSnapshot& x) { return !ch_on(x, 1) && x.busy.empty(); }, 6), "ch1 опущен и выключен");
    check(!w.snapshot().desired_on[0], "команда ch1 сброшена");
    check(ch_on(w.snapshot(), 3), "ch3 не тронут");

    std::printf("\n=== 7. Аварийное выключение посреди разгона ===\n");
    w.set_interlock(true);
    w.request_enable(1, true);
    wait_for(w, [](const PsuSnapshot& x) { return ch_v(x, 1) > 4; }, 3);
    t = timed([&] { w.emergency_shutdown(); });
    check(t < 0.05, "emergency_shutdown() не блокирует");
    check(wait_for(w, [](const PsuSnapshot& x) { return !ch_on(x, 1) && !ch_on(x, 3); }, 3), "все выходы выключены");

    std::printf("\n=== 8. Перезапуск не переключает выходы ===\n");
    w.request_enable(3, true);
    wait_for(w, [](const PsuSnapshot& x) { return ch_on(x, 3); }, 3);
    w.stop();
    {
        PsuWorker w2(config(host, port, 20.0));
        w2.start();
        auto s2 = w2.snapshot();
        check(s2.desired_on[2] && ch_on(s2, 3), "новый worker видит ch3 включённым и считает его приказанным");
        check(!s2.desired_on[0], "ch1 не приказан");
        w2.request_enable(1, true);  // интерлок у нового worker закрыт
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        check(!ch_on(w2.snapshot(), 1), "без интерлока HV по-прежнему не включается");
        w2.request_enable(3, false);
        wait_for(w2, [](const PsuSnapshot& x) { return !ch_on(x, 3); }, 3);
        auto t0 = clk::now();
        w2.stop();
        check(std::chrono::duration<double>(clk::now() - t0).count() < 2, "stop() быстрый");
    }

    std::printf("\n=== Итог: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
