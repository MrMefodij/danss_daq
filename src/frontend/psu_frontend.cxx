// danss_psu_fe — MIDAS frontend блока питания OWON ODP6033 (equipment "PowerSupply").
//
// Запуск:
//   danss_psu_fe [-h mserver_host] [-e experiment] [-- --config config/power_supply.json]
//
// ODB /Equipment/PowerSupply/Settings — источник настроек (решение: «ODB с проверкой»):
//   при первом запуске заполняется из power_supply.json; дальше правится в браузере.
//   Каждое изменение проходит те же проверки, что и JSON (ConfigLoader): пределы прибора,
//   OVP выше цели, OCP не ниже тока, интерлок для SIPM_HV. Недопустимое изменение
//   отклоняется целиком — продолжают действовать предыдущие настройки, в ODB и в
//   сообщениях MIDAS написано, что не так.
//
//   Host, Port, Poll interval sec, Connect timeout ms, Response timeout ms, Max retries
//   Каналы — массивы из 3 элементов, индекс = номер канала - 1:
//     Role ("CPU_POWER" | "SIPM_HV" | "UNUSED"), Name, Target V, Current limit A,
//     OVP V, OCP A, Ramp step V, Ramp interval ms, Require interlock
//   Команды:
//     Enable[3]        — включить/выключить канал (HV — с плавным разгоном/спуском)
//     Interlock ready  — ручной интерлок HV (пока нет интерлока от плат); при старте false
//
// ODB /Equipment/PowerSupply/Readback — состояние (только для чтения):
//   Connected, Protections ok, Busy, Last error, Output on[3], Voltage V[3], Current A[3]
// ODB /Equipment/PowerSupply/Variables/PSUD — то же, что уходит в историю (банк PSUD):
//   V1 V2 V3, I1 I2 I3, P1 P2 P3, On1 On2 On3 (float)
//
// JRPC: "emergency_shutdown" — выключить все выходы сразу, без спуска.
//
// Остановка frontend выходы НЕ трогает: HV, включённое до остановки, остаётся включённым.

#undef NDEBUG  // MIDAS требует assert()

#include "tmfe.h"
#include "midas.h"

#include "common/config_loader.h"
#include "common/constants.h"
#include "common/logger.h"
#include "power/psu_worker.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace danss;
using json = nlohmann::json;

namespace {

constexpr int NCH = 3;
const char* EQ_NAME = constants::EQUIPMENT_NAME_POWER_SUPPLY;

const char* role_name(PSUChannelRole r) {
    return r == PSUChannelRole::CPU_POWER ? "CPU_POWER" : r == PSUChannelRole::SIPM_HV ? "SIPM_HV" : "UNUSED";
}

} // namespace

class EqPowerSupply : public TMFeEquipment {
public:
    std::string fConfigFile = "config/power_supply.json";

    EqPowerSupply(const char* eqname, const char* eqfilename) : TMFeEquipment(eqname, eqfilename) {
        fEqConfEventID = constants::EVENT_ID_POWER_SUPPLY;
        fEqConfBuffer = "SYSTEM";
        fEqConfPeriodMilliSec = 1000;      // опрос ODB-команд раз в секунду; прибор — по Poll interval sec
        fEqConfLogHistory = 1;
        fEqConfWriteEventsToOdb = true;    // банк PSUD -> Variables
        fEqConfReadOnlyWhenRunning = false;  // питание контролируется и вне рана
    }

    ~EqPowerSupply() override {
        if (fWorker) {
            fWorker->stop();  // выходы не трогаем
            fWorker.reset();
        }
    }

    // ------------------------------------------------------------------ init

    TMFeResult HandleInit(const std::vector<std::string>& /*args*/) override {
        EqSetStatus("Starting...", "yellowLight");

        std::string host;
        fOdbEqSettings->RS("Host", &host);
        if (host.empty()) {
            // Первый запуск: заполнить ODB из файла (он же проверяется ConfigLoader)
            PSUConfig file_cfg;
            try {
                file_cfg = ConfigLoader::load_power_config(fConfigFile);
            } catch (const ConfigError& e) {
                return TMFeErrorMessage(std::string("Cannot seed ODB settings: ") + e.what());
            }
            WriteSettings(file_cfg);
            fMfe->Msg(MINFO, "HandleInit", "PowerSupply: ODB settings created from %s", fConfigFile.c_str());
        }
        fOdbEqSettings->WS("Config file", fConfigFile.c_str());

        PSUConfig cfg;
        std::string text = SettingsJson();
        try {
            cfg = ConfigLoader::parse_power_config(text, "ODB " + Path());
        } catch (const ConfigError& e) {
            EqSetStatus("Invalid settings", "redLight");
            return TMFeErrorMessage(std::string("Invalid ODB settings: ") + e.what());
        }
        fAppliedJson = text;

        // Ручной интерлок после перезапуска закрыт: оператор подтверждает заново
        fOdbEqSettings->WB("Interlock ready", false);
        fLastInterlock = false;

        std::vector<std::string> names = {"V1 [V]", "V2 [V]", "V3 [V]", "I1 [A]", "I2 [A]", "I3 [A]",
                                          "P1 [W]", "P2 [W]", "P3 [W]", "On1", "On2", "On3"};
        fOdbEqSettings->WSA((std::string("Names ") + constants::BANK_NAME_PSU_DATA).c_str(), names, 32);

        fOdbReadback = fOdbEq->Chdir("Readback", true);
        fOdbReadback->WS("Settings error", "", 256);

        fWorker = std::make_unique<PsuWorker>(cfg);
        fWorker->start();

        // Enable[] — то, что сейчас на приборе (worker ничего не переключает при старте)
        PsuSnapshot s = fWorker->snapshot();
        fLastEnable.assign(s.desired_on.begin(), s.desired_on.end());
        fOdbEqSettings->WBA("Enable", fLastEnable);
        if (!s.connected)
            fMfe->Msg(MERROR, "HandleInit", "PowerSupply: no connection to %s:%u — will keep retrying",
                      cfg.host.c_str(), cfg.port);

        fMfe->Msg(MINFO, "HandleInit", "PowerSupply: started, %s:%u, outputs on: %d %d %d", cfg.host.c_str(),
                  cfg.port, (int)s.desired_on[0], (int)s.desired_on[1], (int)s.desired_on[2]);
        return TMFeOk();
    }

    // -------------------------------------------------------------- periodic

    void HandlePeriodic() override {
        if (!fWorker) return;
        CheckSettings();
        CheckCommands();
        Publish();
    }

    // ------------------------------------------------------------------ rpc

    TMFeResult HandleRpc(const char* cmd, const char* /*args*/, std::string& response) override {
        if (std::string(cmd) == "emergency_shutdown" && fWorker) {
            fMfe->Msg(MERROR, "HandleRpc", "PowerSupply: EMERGENCY SHUTDOWN requested via RPC");
            fWorker->emergency_shutdown();
            response = "ok";
        }
        return TMFeOk();
    }

private:
    std::unique_ptr<PsuWorker> fWorker;
    MVOdb* fOdbReadback = nullptr;
    std::string fAppliedJson, fRejectedJson;
    std::vector<bool> fLastEnable;
    bool fLastInterlock = false;
    uint64_t fLastSeq = 0, fLastErrorSeq = 0;
    bool fLinkAlarm = false;
    std::vector<bool> fVoltageAlarm = std::vector<bool>(NCH, false);

    std::string Path() const { return std::string("/Equipment/") + EQ_NAME + "/Settings"; }

    // ---- ODB <-> PSUConfig

    void WriteSettings(const PSUConfig& c) {
        fOdbEqSettings->WS("Host", c.host.c_str(), 64);
        fOdbEqSettings->WI("Port", c.port);
        fOdbEqSettings->WI("Poll interval sec", (int)c.poll_interval_sec);
        fOdbEqSettings->WI("Connect timeout ms", (int)c.connect_timeout_ms);
        fOdbEqSettings->WI("Response timeout ms", (int)c.response_timeout_ms);
        fOdbEqSettings->WI("Max retries", c.max_retries);

        std::vector<std::string> role(NCH, "UNUSED"), name(NCH);
        std::vector<double> v(NCH, 0), i(NCH, 0), ovp(NCH, 0), ocp(NCH, 0), step(NCH, constants::DEFAULT_PSU_RAMP_STEP_V);
        std::vector<int> interval(NCH, (int)constants::DEFAULT_PSU_RAMP_INTERVAL_MS);
        std::vector<bool> interlock(NCH, false);
        for (int k = 0; k < NCH; ++k) name[k] = "ch" + std::to_string(k + 1);
        for (const auto& ch : c.channels) {
            int k = ch.channel - 1;
            role[k] = role_name(ch.role);
            name[k] = ch.name;
            v[k] = ch.target_voltage_v;
            i[k] = ch.current_limit_a;
            ovp[k] = ch.ovp_limit_v;
            ocp[k] = ch.ocp_limit_a;
            step[k] = ch.ramp_step_v;
            interval[k] = (int)ch.ramp_interval_ms;
            interlock[k] = ch.require_interlock;
        }
        fOdbEqSettings->WSA("Role", role, 16);
        fOdbEqSettings->WSA("Name", name, 64);
        fOdbEqSettings->WDA("Target V", v);
        fOdbEqSettings->WDA("Current limit A", i);
        fOdbEqSettings->WDA("OVP V", ovp);
        fOdbEqSettings->WDA("OCP A", ocp);
        fOdbEqSettings->WDA("Ramp step V", step);
        fOdbEqSettings->WIA("Ramp interval ms", interval);
        fOdbEqSettings->WBA("Require interlock", interlock);
        fOdbEqSettings->WBA("Enable", std::vector<bool>(NCH, false));
        fOdbEqSettings->WB("Interlock ready", false);
    }

    /// Settings из ODB в текст power_supply.json — дальше та же проверка, что и для файла
    std::string SettingsJson() {
        json ps;
        std::string host;
        int port = 0, poll = 0, ct = 0, rt = 0, retries = 0;
        fOdbEqSettings->RS("Host", &host);
        fOdbEqSettings->RI("Port", &port);
        fOdbEqSettings->RI("Poll interval sec", &poll);
        fOdbEqSettings->RI("Connect timeout ms", &ct);
        fOdbEqSettings->RI("Response timeout ms", &rt);
        fOdbEqSettings->RI("Max retries", &retries);
        ps["host"] = host;
        ps["port"] = port;
        ps["poll_interval_sec"] = poll;
        ps["connect_timeout_ms"] = ct;
        ps["response_timeout_ms"] = rt;
        ps["max_retries"] = retries;

        std::vector<std::string> role, name;
        std::vector<double> v, i, ovp, ocp, step;
        std::vector<int> interval;
        std::vector<bool> interlock;
        fOdbEqSettings->RSA("Role", &role);
        fOdbEqSettings->RSA("Name", &name);
        fOdbEqSettings->RDA("Target V", &v);
        fOdbEqSettings->RDA("Current limit A", &i);
        fOdbEqSettings->RDA("OVP V", &ovp);
        fOdbEqSettings->RDA("OCP A", &ocp);
        fOdbEqSettings->RDA("Ramp step V", &step);
        fOdbEqSettings->RIA("Ramp interval ms", &interval);
        fOdbEqSettings->RBA("Require interlock", &interlock);

        ps["channels"] = json::array();
        for (size_t k = 0; k < role.size() && k < (size_t)NCH; ++k) {
            json ch = {{"channel", k + 1}, {"role", role[k]}};
            if (k < name.size() && !name[k].empty()) ch["name"] = name[k];
            if (k < v.size()) ch["target_voltage_v"] = v[k];
            if (k < i.size()) ch["current_limit_a"] = i[k];
            if (k < ovp.size()) ch["ovp_limit_v"] = ovp[k];
            if (k < ocp.size()) ch["ocp_limit_a"] = ocp[k];
            if (k < step.size()) ch["ramp_step_v"] = step[k];
            if (k < interval.size()) ch["ramp_interval_ms"] = interval[k];
            if (k < interlock.size()) ch["require_interlock"] = (bool)interlock[k];
            ps["channels"].push_back(ch);
        }
        return json{{"power_supply", ps}}.dump();
    }

    // ---- изменения настроек

    void CheckSettings() {
        std::string text = SettingsJson();
        if (text == fAppliedJson || text == fRejectedJson) return;
        try {
            PSUConfig cfg = ConfigLoader::parse_power_config(text, "ODB " + Path());
            fWorker->request_config(cfg);
            fAppliedJson = text;
            fRejectedJson.clear();
            fOdbReadback->WS("Settings error", "", 256);
            fMfe->ResetAlarm("PowerSupply settings");
            fMfe->Msg(MINFO, "CheckSettings", "PowerSupply: new settings accepted, protection limits rewritten");
        } catch (const ConfigError& e) {
            fRejectedJson = text;
            fOdbReadback->WS("Settings error", e.what(), 256);
            fMfe->Msg(MERROR, "CheckSettings", "PowerSupply: settings change REJECTED, previous settings stay: %s",
                      e.what());
            fMfe->TriggerAlarm("PowerSupply settings", "PowerSupply: invalid settings rejected, see Readback/Settings error",
                               "Warning");
        }
    }

    // ---- команды Enable[] и Interlock ready

    void CheckCommands() {
        bool interlock = false;
        fOdbEqSettings->RB("Interlock ready", &interlock);
        if (interlock != fLastInterlock) {
            fLastInterlock = interlock;
            // MINFO/MERROR — макросы с __FILE__/__LINE__ внутри, в тернарный оператор их не подставить
            if (interlock)
                fMfe->Msg(MINFO, "CheckCommands", "PowerSupply: interlock READY (manual, ODB)");
            else
                fMfe->Msg(MERROR, "CheckCommands", "PowerSupply: interlock DROPPED (manual, ODB) — interlocked channels ramp down");
            fWorker->set_interlock(interlock);
        }

        std::vector<bool> enable;
        fOdbEqSettings->RBA("Enable", &enable);
        enable.resize(NCH, false);
        for (int k = 0; k < NCH; ++k) {
            if (enable[k] != fLastEnable[k]) {
                fMfe->Msg(MINFO, "CheckCommands", "PowerSupply: ch%d %s requested", k + 1, enable[k] ? "ON" : "OFF");
                fWorker->request_enable(k + 1, enable[k]);
            }
        }
        fLastEnable = enable;
    }

    // ---- состояние в ODB, событие, алармы, статус

    void Publish() {
        PsuSnapshot s = fWorker->snapshot();

        // Worker отменил команду (нет интерлока, ошибка) — показать это в Enable[]
        std::vector<bool> desired(s.desired_on.begin(), s.desired_on.end());
        if (desired != fLastEnable) {
            fOdbEqSettings->WBA("Enable", desired);
            fLastEnable = desired;
        }

        if (s.error_seq != fLastErrorSeq) {
            fLastErrorSeq = s.error_seq;
            fMfe->Msg(MERROR, "Publish", "PowerSupply: %s", s.last_error.c_str());
        }

        std::vector<bool> on(NCH, false);
        std::vector<double> v(NCH, 0), i(NCH, 0), p(NCH, 0);
        for (const auto& c : s.channels) {
            if (c.channel < 1 || c.channel > NCH || !c.data_valid) continue;
            int k = c.channel - 1;
            on[k] = c.output_enabled;
            v[k] = c.measured_voltage_v;
            i[k] = c.measured_current_a;
            p[k] = c.measured_power_w;
        }

        fOdbReadback->WB("Connected", s.connected);
        fOdbReadback->WB("Protections ok", s.protections_ok);
        fOdbReadback->WB("Interlock ready", s.interlock_ready);
        fOdbReadback->WS("Busy", s.busy.c_str(), 128);
        fOdbReadback->WS("Last error", s.last_error.c_str(), 256);
        fOdbReadback->WBA("Output on", on);
        fOdbReadback->WDA("Voltage V", v);
        fOdbReadback->WDA("Current A", i);

        if (s.seq != fLastSeq) {
            fLastSeq = s.seq;
            SendEvent(v, i, p, on);
            CheckAlarms(s, on, v);
        }
        SetStatus(s, on, v);
    }

    void SendEvent(const std::vector<double>& v, const std::vector<double>& i, const std::vector<double>& p,
                   const std::vector<bool>& on) {
        char buf[1024];
        ComposeEvent(buf, sizeof(buf));
        BkInit(buf, sizeof(buf));
        float* d = (float*)BkOpen(buf, constants::BANK_NAME_PSU_DATA, TID_FLOAT);
        for (int k = 0; k < NCH; ++k) *d++ = (float)v[k];
        for (int k = 0; k < NCH; ++k) *d++ = (float)i[k];
        for (int k = 0; k < NCH; ++k) *d++ = (float)p[k];
        for (int k = 0; k < NCH; ++k) *d++ = on[k] ? 1.0f : 0.0f;
        BkClose(buf, d);
        EqSendEvent(buf);
    }

    void CheckAlarms(const PsuSnapshot& s, const std::vector<bool>& on, const std::vector<double>& v) {
        if (!s.connected && !fLinkAlarm) {
            fLinkAlarm = true;
            fMfe->TriggerAlarm("PowerSupply link", ("PowerSupply: no connection to " + s.config.host).c_str(), "Alarm");
        } else if (s.connected && fLinkAlarm) {
            fLinkAlarm = false;
            fMfe->ResetAlarm("PowerSupply link");
        }
        // Включённый канал не на своём напряжении (вне разгона) — аларм
        for (const auto& c : s.config.channels) {
            int k = c.channel - 1;
            if (k < 0 || k >= NCH || c.role == PSUChannelRole::UNUSED) continue;
            bool bad = s.connected && s.busy.empty() && on[k] && s.desired_on[k]
                       && std::fabs(v[k] - c.target_voltage_v) > constants::PSU_VOLTAGE_TOLERANCE_V;
            std::string name = "PowerSupply ch" + std::to_string(c.channel) + " voltage";
            if (bad && !fVoltageAlarm[k]) {
                fVoltageAlarm[k] = true;
                char msg[256];
                snprintf(msg, sizeof(msg), "PowerSupply ch%d (%s): %.3f V, expected %.3f V", c.channel, c.name.c_str(),
                         v[k], c.target_voltage_v);
                fMfe->TriggerAlarm(name.c_str(), msg, "Alarm");
            } else if (!bad && fVoltageAlarm[k]) {
                fVoltageAlarm[k] = false;
                fMfe->ResetAlarm(name.c_str());
            }
        }
    }

    void SetStatus(const PsuSnapshot& s, const std::vector<bool>& on, const std::vector<double>& v) {
        if (!s.connected) {
            EqSetStatus(("No connection to " + s.config.host).c_str(), "redLight");
            return;
        }
        std::string text;
        for (int k = 0; k < NCH; ++k) {
            if (!on[k]) continue;
            char b[48];
            snprintf(b, sizeof(b), "%sch%d %.2f V", text.empty() ? "" : ", ", k + 1, v[k]);
            text += b;
        }
        if (text.empty()) text = "All outputs off";
        if (!s.busy.empty()) {
            EqSetStatus((s.busy + " | " + text).c_str(), "yellowLight");
        } else if (!s.protections_ok) {
            EqSetStatus(("OVP/OCP not confirmed | " + text).c_str(), "redLight");
        } else {
            EqSetStatus(text.c_str(), "greenLight");
        }
    }
};

class FePowerSupply : public TMFrontend {
public:
    EqPowerSupply* fEq = nullptr;

    FePowerSupply() {
        FeSetName("danss_psu_fe");
        fEq = new EqPowerSupply(EQ_NAME, __FILE__);
        FeAddEquipment(fEq);
    }

    TMFeResult HandleArguments(const std::vector<std::string>& args) override {
        for (size_t k = 0; k < args.size(); ++k) {
            if ((args[k] == "--config" || args[k] == "-c") && k + 1 < args.size()) fEq->fConfigFile = args[++k];
            else return TMFeErrorMessage("unknown argument after --: " + args[k]);
        }
        return TMFeOk();
    }

    void HandleUsage() override {
        printf("PowerSupply options (after --):\n");
        printf("  --config <file>   power_supply.json used to seed ODB on first start (default config/power_supply.json)\n");
    }

    TMFeResult HandleFrontendReady(const std::vector<std::string>& /*args*/) override {
        // Периодический обработчик — в своём потоке: MIDAS-поток (переходы, RPC) не ждёт ODB/прибор
        FeStartPeriodicThread();
        fMfe->StartRpcThread();
        return TMFeOk();
    }

    void HandleFrontendExit() override {
        fMfe->Msg(MINFO, "HandleFrontendExit", "PowerSupply frontend stopped, outputs left unchanged");
    }
};

int main(int argc, char* argv[]) {
    FePowerSupply fe;
    return fe.FeMain(argc, argv);
}
