// Пошаговая проверка OwonPSU / PowerSupplyController на реальном блоке OWON ODP6033.
//
// В отличие от test_owon_psu (поднимает HV до 55 В и в конце выключает ВСЕ
// выходы, включая питание электроники), здесь каждый шаг запускается отдельно
// и HV ограничено низким напряжением.
//
//   psu_bench info                 только чтение: IDN, выходы, измерения, OVP/OCP
//   psu_bench limits               записать OVP/OCP/ток из конфигурации и проверить чтением
//   psu_bench hv <V>               ch1: разгон 0 -> V, выдержка, плавный спуск, выключение
//   psu_bench interlock <V>        ch1: разгон к V, на середине снимается интерлок ->
//                                  разгон должен прерваться, канал опуститься и выключиться
//
// Опции: --host <h> (192.168.10.81 = d2pwr1.danss.local), --port <p> (3000), --allow-high (разрешить V > 20).
// Канал 3 (питание электроники) не выключается ни в одном режиме.

#include "common/types.h"
#include "common/logger.h"
#include "power/owon_psu.h"
#include "power/power_controller.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace danss;

namespace {

constexpr double HV_SAFE_MAX_V = 20.0;

PSUConfig make_config(const std::string& host, uint16_t port, double hv_target_v) {
    PSUConfig cfg;
    cfg.host = host;
    cfg.port = port;
    cfg.poll_interval_sec = 300;
    cfg.connect_timeout_ms = 2000;
    cfg.response_timeout_ms = 1000;
    cfg.max_retries = 3;

    // Значения ch3 — как в config/power_supply.json
    PSUChannelConfig cpu{};
    cpu.channel = 3;
    cpu.role = PSUChannelRole::CPU_POWER;
    cpu.name = "Electronics boards 6V";
    cpu.target_voltage_v = 6.0;
    cpu.current_limit_a = 3.0;
    cpu.ovp_limit_v = 6.5;
    cpu.ocp_limit_a = 3.2;
    cpu.ramp_step_v = 1.0;
    cpu.ramp_interval_ms = 500;
    cpu.require_interlock = false;

    // ch1 — HV, но с низкой целью и OVP чуть выше неё
    PSUChannelConfig hv{};
    hv.channel = 1;
    hv.role = PSUChannelRole::SIPM_HV;
    hv.name = "SiPM bias A (bench)";
    hv.target_voltage_v = hv_target_v;
    hv.current_limit_a = 0.5;
    hv.ovp_limit_v = hv_target_v + 2.0;
    hv.ocp_limit_a = 0.7;
    hv.ramp_step_v = 1.0;
    hv.ramp_interval_ms = 500;
    hv.require_interlock = true;

    cfg.channels = {cpu, hv};
    return cfg;
}

void print_poll(PowerSupplyController& ctrl, const char* tag) {
    for (const auto& s : ctrl.poll()) {
        std::printf("  [%s] ch%u: %s  V=%.3f  I=%.4f  P=%.3f  out=%s\n", tag, s.channel,
                    s.data_valid ? "ok " : "ERR", s.measured_voltage_v, s.measured_current_a,
                    s.measured_power_w, s.output_enabled ? "ON" : "off");
        if (!s.data_valid) std::printf("        %s\n", s.error_message.c_str());
    }
}

bool step(bool ok, const std::string& what, const std::string& err = "") {
    std::printf("%s %s%s%s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str(),
                (!ok && !err.empty()) ? " — " : "", (!ok ? err.c_str() : ""));
    return ok;
}

int mode_info(const std::string& host, uint16_t port) {
    OwonPSU psu(host, port);
    psu.set_response_timeout(1000);
    if (!step(psu.connect(), "connect " + host + ":" + std::to_string(port), psu.get_last_error())) return 1;

    std::string idn;
    if (step(psu.get_idn(idn), "*IDN?", psu.get_last_error())) std::printf("       %s\n", idn.c_str());

    std::array<bool, 3> on{};
    if (step(psu.get_all_output_states(on), "OUTPut:STATe? по каналам (нужен для проверки питания электроники перед HV)",
             psu.get_last_error()))
        std::printf("       outputs: ch1=%d ch2=%d ch3=%d\n", on[0], on[1], on[2]);

    std::array<double, 3> v{}, i{};
    if (step(psu.measure_all_voltages(v), "MEASure:VOLTage:ALL:DC?", psu.get_last_error()))
        std::printf("       V: %.3f %.3f %.3f\n", v[0], v[1], v[2]);
    if (step(psu.measure_all_currents(i), "MEASure:CURRent:ALL:DC?", psu.get_last_error()))
        std::printf("       I: %.4f %.4f %.4f\n", i[0], i[1], i[2]);

    for (uint8_t ch = 1; ch <= 3; ++ch) {
        double ovp = 0, ocp = 0;
        bool ok = psu.select_channel(ch) && psu.get_voltage_protection(ovp) && psu.get_current_protection(ocp);
        if (step(ok, "ch" + std::to_string(ch) + " OVP/OCP read back", psu.get_last_error()))
            std::printf("       OVP=%.3f V  OCP=%.3f A\n", ovp, ocp);
    }
    psu.disconnect();
    return 0;
}

int mode_limits(const std::string& host, uint16_t port) {
    PowerSupplyController ctrl(make_config(host, port, 5.0));
    if (!step(ctrl.connect(), "connect + SYSTem:REMote", ctrl.get_last_error())) return 1;
    bool ok = step(ctrl.apply_protection_limits(),
                   "apply_protection_limits(): ch3 OVP 6.5/OCP 3.2, ch1 OVP 7.0/OCP 0.7", ctrl.get_last_error());
    print_poll(ctrl, "after");
    ctrl.disconnect();
    return ok ? 0 : 1;
}

int mode_hv(const std::string& host, uint16_t port, double target, bool drop_interlock) {
    PowerSupplyController ctrl(make_config(host, port, target));
    if (!step(ctrl.connect(), "connect + SYSTem:REMote", ctrl.get_last_error())) return 1;
    if (!step(ctrl.apply_protection_limits(), "apply_protection_limits()", ctrl.get_last_error())) return 1;
    print_poll(ctrl, "start");

    step(!ctrl.enable_channel(1), "ch1 отклонён без интерлока (ожидаемо)", "HV включился без интерлока!");
    ctrl.set_interlock_ready(true);

    if (!drop_interlock) {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = step(ctrl.enable_channel(1), "enable_channel(1): разгон 0 -> " + std::to_string(target) + " В",
                       ctrl.get_last_error());
        std::printf("       разгон занял %.1f с\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        for (int k = 0; k < 3; ++k) {
            print_poll(ctrl, "hold");
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        bool off = step(ctrl.disable_channel(1), "disable_channel(1): плавный спуск и выключение",
                        ctrl.get_last_error());
        print_poll(ctrl, "end");
        ctrl.disconnect();
        return (ok && off) ? 0 : 1;
    }

    // Снятие интерлока посреди разгона: разгон идёт в отдельном потоке
    bool enable_result = true;
    std::thread ramp([&] { enable_result = ctrl.enable_channel(1); });
    double ramp_s = target * 0.5 * 0.5;  // половина пути при 1 В / 0.5 с
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(ramp_s * 1000)));
    std::printf("       снимаю интерлок через %.1f с после начала разгона\n", ramp_s);
    ctrl.set_interlock_ready(false);  // ждёт остановки разгона, затем спускает и выключает ch1
    ramp.join();
    step(!enable_result, "enable_channel(1) прервался", "разгон дошёл до цели, хотя интерлок сняли");
    print_poll(ctrl, "end");
    bool ch1_off = true;
    for (const auto& s : ctrl.poll())
        if (s.channel == 1 && s.output_enabled) ch1_off = false;
    step(ch1_off, "ch1 выключен после снятия интерлока", "ch1 ОСТАЛСЯ ВКЛЮЧЁН");
    ctrl.disconnect();
    return (!enable_result && ch1_off) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "192.168.10.81";  // d2pwr1.danss.local: Boost.Asio на cvme1 имя не резолвит
    uint16_t port = 3000;
    bool allow_high = false;
    std::string mode;
    double volts = 0.0;

    for (int a = 1; a < argc; ++a) {
        std::string s = argv[a];
        if (s == "--host" && a + 1 < argc) host = argv[++a];
        else if (s == "--port" && a + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++a]));
        else if (s == "--allow-high") allow_high = true;
        else if (mode.empty()) mode = s;
        else volts = std::stod(s);
    }

    danss::Logger::instance().set_level(constants::LogLevel::INFO);

    if (mode == "info") return mode_info(host, port);
    if (mode == "limits") return mode_limits(host, port);
    if (mode == "hv" || mode == "interlock") {
        if (volts <= 0.0) { std::fprintf(stderr, "Укажите напряжение, например: psu_bench %s 5\n", mode.c_str()); return 2; }
        if (volts > HV_SAFE_MAX_V && !allow_high) {
            std::fprintf(stderr, "%.1f В > %.0f В: нужен флаг --allow-high\n", volts, HV_SAFE_MAX_V);
            return 2;
        }
        return mode_hv(host, port, volts, mode == "interlock");
    }
    std::fprintf(stderr, "Режимы: info | limits | hv <V> | interlock <V>  [--host h] [--port p] [--allow-high]\n");
    return 2;
}
