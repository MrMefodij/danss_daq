// Автономный тест-харнесс для OwonPSU / PowerSupplyController.
//
// Не завязан на CMake/MIDAS — собирается напрямую через g++ (см. README в
// этом же каталоге / инструкцию в чате). По умолчанию бьёт в mock-сервер
// на localhost, но host/port можно передать аргументами, чтобы прогнать
// тот же тест на реальном блоке OWON ODP6033 на стенде:
//
//   ./test_owon_psu d2pwr1.danss.local 3000
//
// Тест меняет реальное состояние блока питания (включает/выключает
// каналы, поднимает HV до 55В!) — на реальном железе запускать только
// когда стенд к этому готов (см. предупреждение перед HV-частью).

#include "common/types.h"
#include "power/owon_psu.h"
#include "power/power_controller.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace danss;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  [PASS] %s\n", what.c_str());
        g_pass++;
    } else {
        std::printf("  [FAIL] %s\n", what.c_str());
        g_fail++;
    }
}

bool approx(double a, double b, double tol = 0.05) {
    return std::fabs(a - b) <= tol;
}

PSUChannelConfig make_channel(uint8_t ch, PSUChannelRole role, const std::string& name,
                               double target_v, bool interlock) {
    PSUChannelConfig c{};
    c.channel = ch;
    c.role = role;
    c.name = name;
    c.target_voltage_v = target_v;
    c.current_limit_a = (role == PSUChannelRole::SIPM_HV) ? 0.5 : 3.0;
    c.ovp_limit_v = target_v + 2.0;
    c.ocp_limit_a = c.current_limit_a + 0.2;
    c.ramp_step_v = 5.0;      // крупный шаг — тест не должен ждать долго
    c.ramp_interval_ms = 20;  // короткий интервал — тест не должен ждать долго
    c.require_interlock = interlock;
    return c;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 15000;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));

    std::printf("=== 1. Низкоуровневый OwonPSU: подключение и *IDN? ===\n");
    OwonPSU psu(host, port);
    psu.set_response_timeout(2000);
    check(psu.connect(), "connect() к " + host + ":" + std::to_string(port));

    std::string idn;
    bool idn_ok = psu.get_idn(idn);
    check(idn_ok, "*IDN? вернул ответ");
    if (idn_ok) {
        std::printf("       IDN: %s\n", idn.c_str());
        check(idn.find("OWON") != std::string::npos, "*IDN? содержит 'OWON'");
    }
    psu.disconnect();

    std::printf("\n=== 2. PowerSupplyController: конфигурация из ТЗ (CH3=CPU 6V, CH1=SiPM HV 55V) ===\n");

    PSUConfig config;
    config.host = host;
    config.port = port;
    config.poll_interval_sec = 300;
    config.connect_timeout_ms = 2000;
    config.response_timeout_ms = 2000;
    config.max_retries = 3;
    config.channels = {
        make_channel(3, PSUChannelRole::CPU_POWER, "Electronics 6V", 6.0, /*interlock=*/false),
        make_channel(1, PSUChannelRole::SIPM_HV, "SiPM bias A", 55.0, /*interlock=*/true),
    };

    PowerSupplyController ctrl(config);
    check(ctrl.connect(), "PowerSupplyController::connect()");
    check(ctrl.apply_protection_limits(), "apply_protection_limits() (OVP/OCP на все каналы)");

    std::printf("\n=== 3. Интерлок блокирует включение SiPM HV ===\n");
    check(!ctrl.is_interlock_ready(), "интерлок изначально не готов");
    bool hv_enable_blocked = !ctrl.enable_channel(1);
    check(hv_enable_blocked, "enable_channel(1) [SIPM_HV] отклонён без интерлока");

    std::printf("\n=== 4. CPU_POWER канал включается без интерлока, напряжение сразу на целевом ===\n");
    check(ctrl.enable_channel(3), "enable_channel(3) [CPU_POWER] прошёл");
    {
        auto statuses = ctrl.poll();
        bool found = false;
        for (const auto& s : statuses) {
            if (s.channel == 3) {
                found = true;
                check(s.data_valid, "poll(): ch3 data_valid");
                check(s.output_enabled, "poll(): ch3 output_enabled == true");
                check(approx(s.measured_voltage_v, 6.0), "poll(): ch3 measured ~= 6.0V (получено "
                      + std::to_string(s.measured_voltage_v) + ")");
            }
        }
        check(found, "poll(): статус ch3 присутствует в результате");
    }

    std::printf("\n=== 5. После снятия интерлока SiPM HV разгоняется до цели ===\n");
    ctrl.set_interlock_ready(true);
    check(ctrl.is_interlock_ready(), "интерлок теперь готов");
    check(ctrl.enable_channel(1), "enable_channel(1) [SIPM_HV] прошёл с интерлоком");
    {
        auto statuses = ctrl.poll();
        for (const auto& s : statuses) {
            if (s.channel == 1) {
                check(s.data_valid, "poll(): ch1 data_valid");
                check(s.output_enabled, "poll(): ch1 output_enabled == true");
                check(approx(s.measured_voltage_v, 55.0), "poll(): ch1 measured ~= 55.0V (получено "
                      + std::to_string(s.measured_voltage_v) + ")");
            }
        }
    }

    std::printf("\n=== 6. disable_channel() плавно опускает HV и выключает выход ===\n");
    check(ctrl.disable_channel(1), "disable_channel(1) [SIPM_HV] прошёл");
    {
        auto statuses = ctrl.poll();
        for (const auto& s : statuses) {
            if (s.channel == 1) {
                check(!s.output_enabled, "poll(): ch1 output_enabled == false после disable");
            }
        }
    }

    std::printf("\n=== 7. emergency_shutdown() выключает всё немедленно ===\n");
    ctrl.enable_channel(3); // снова включим что-то, чтобы было что выключать аварийно
    ctrl.emergency_shutdown();
    {
        auto statuses = ctrl.poll();
        bool all_off = true;
        for (const auto& s : statuses) {
            if (s.output_enabled) all_off = false;
        }
        check(all_off, "emergency_shutdown(): все каналы выключены");
    }

    ctrl.disconnect();

    std::printf("\n=== Итог: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
