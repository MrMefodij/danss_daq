// danss_sc_fe — MIDAS frontend опроса плат электроники SiPM (equipment "SlowControl").
//
// Запуск:
//   danss_sc_fe [-h mserver_host] [-e experiment] [-- --config config/board_map.json]
//
// Платы и ветки RS-485 берутся из board_map.json (проверяется ConfigLoader). Опрос —
// PeriodicPoller (поток на ветку), состояние — BoardRegistry, проверки — DataCollector.
//
// ODB /Equipment/SlowControl:
//   Settings/Config file        — откуда взяты платы (для справки)
//   Settings/Poll interval sec  — период опроса; меняется на лету
//   Settings/Poll now           — y: опросить сейчас (сбрасывается сам)
//   Settings/Names BDAT|SIPI|SIPT — подписи для истории
//   Readback/                   — сводка: Online, Stale, Offline, Disabled, With problems,
//                                 Tripped, All healthy, Last cycle (время)
//   Boards/<имя>/               — по плате: Status, Branch, Address, Update counter,
//                                 Analog boards, Problems, Last error, Reboots, Failures
//   Variables/BDAT              — плата × 8: status(0 unknown 1 online 2 stale 3 offline 4 disabled),
//                                 update counter, CPU temp, HV status, tripped mask,
//                                 analog mask, DAC mismatches, reboots
//   Variables/SIPI, SIPT        — плата × 12: токи и температуры SiPM (raw; -1 — нет датчика)
//
// Алармы (класс Alarm): «SC <плата> offline», «SC <плата> stale», «SC <плата> trip»,
// «SC <плата> DAC»; перезагрузка платы — сообщение MIDAS, не аларм.
// «All healthy» — кандидат в источник интерлока HV для danss_psu_fe (условия — решение группы).
// JRPC: "poll_now".

#undef NDEBUG  // MIDAS требует assert()

#include "tmfe.h"
#include "midas.h"

#include "common/config_loader.h"
#include "common/constants.h"
#include "common/logger.h"
#include "rs485/board_registry.h"
#include "slowcontrol/periodic_poller.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace danss;
using namespace danss::constants;

namespace {

const char* EQ_NAME = EQUIPMENT_NAME_SLOW_CONTROL;

double status_code(BoardStatus s) {
    switch (s) {
        case BoardStatus::UNKNOWN: return 0;
        case BoardStatus::ONLINE: return 1;
        case BoardStatus::STALE: return 2;
        case BoardStatus::OFFLINE: return 3;
        case BoardStatus::DISABLED: return 4;
    }
    return 0;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : "; ") + s;
    return out;
}

/// Имя ключа ODB из имени платы: без '/'
std::string odb_key(std::string s) {
    for (auto& c : s)
        if (c == '/') c = '_';
    return s;
}

} // namespace

class EqSlowControl : public TMFeEquipment {
public:
    std::string fConfigFile = "config/board_map.json";

    EqSlowControl(const char* eqname, const char* eqfilename) : TMFeEquipment(eqname, eqfilename) {
        fEqConfEventID = EVENT_ID_SLOW_CONTROL;
        fEqConfBuffer = "SYSTEM";
        fEqConfPeriodMilliSec = 1000;        // проверка «цикл опроса закончился» раз в секунду
        fEqConfLogHistory = 1;
        fEqConfWriteEventsToOdb = true;      // банки -> Variables
        fEqConfReadOnlyWhenRunning = false;  // платы контролируются и вне рана
    }

    ~EqSlowControl() override {
        if (fPoller) fPoller->stop();
    }

    TMFeResult HandleInit(const std::vector<std::string>& /*args*/) override {
        EqSetStatus("Starting...", "yellowLight");
        try {
            fConfig = ConfigLoader::load_system_config(fConfigFile);
        } catch (const ConfigError& e) {
            EqSetStatus("Invalid board_map", "redLight");
            return TMFeErrorMessage(std::string("Invalid board configuration: ") + e.what());
        }

        fRegistry = std::make_unique<BoardRegistry>(fConfig);
        for (const auto& b : fRegistry->snapshot()) fOrder.push_back({b.branch_id, b.config.address});

        fOdbEqSettings->WS("Config file", fConfigFile.c_str(), 256);
        int interval = static_cast<int>(fConfig.poll_interval_sec);
        fOdbEqSettings->RI("Poll interval sec", &interval, true);  // создать, если нет; иначе взять из ODB
        if (interval < 1) interval = 1;
        fLastInterval = interval;
        fOdbEqSettings->WB("Poll now", false);
        WriteNames();

        fOdbReadback = fOdbEq->Chdir("Readback", true);
        fOdbBoards = fOdbEq->Chdir("Boards", true);

        fPoller = std::make_unique<PeriodicPoller>(fConfig, *fRegistry);
        fPoller->set_poll_interval(interval);
        fPoller->set_cycle_callback([this](uint8_t) { fCycleDone = true; });
        fPoller->start();

        size_t enabled = 0;
        for (const auto& br : fConfig.branches)
            for (const auto& b : br.boards) enabled += b.enabled;
        fMfe->Msg(MINFO, "HandleInit", "SlowControl: %zu branch(es), %zu board(s) (%zu enabled), poll every %d s",
                  fConfig.branches.size(), fOrder.size(), enabled, interval);
        return TMFeOk();
    }

    void HandlePeriodic() override {
        if (!fPoller) return;
        CheckSettings();
        if (fCycleDone.exchange(false)) Publish();
    }

    TMFeResult HandleRpc(const char* cmd, const char* /*args*/, std::string& response) override {
        if (std::string(cmd) == "poll_now" && fPoller) {
            fPoller->poll_now();
            response = "ok";
        }
        return TMFeOk();
    }

private:
    SystemConfig fConfig;
    std::unique_ptr<BoardRegistry> fRegistry;
    std::unique_ptr<PeriodicPoller> fPoller;
    std::vector<BoardRegistry::Key> fOrder;  // порядок плат в банках = порядок в конфиге
    std::atomic<bool> fCycleDone{false};
    MVOdb* fOdbReadback = nullptr;
    MVOdb* fOdbBoards = nullptr;
    int fLastInterval = 0;

    struct AlarmState {
        bool offline = false, stale = false, trip = false, dac = false;
        uint64_t reboots = 0;
        BoardStatus status = BoardStatus::UNKNOWN;
    };
    std::map<BoardRegistry::Key, AlarmState> fAlarms;

    void WriteNames() {
        std::vector<std::string> bdat, sipi, sipt;
        const char* fields[BDAT_VALUES_PER_BOARD] = {"status", "counter", "CPU T", "HV st", "trip", "analog", "DAC!", "reboots"};
        for (const auto& b : fRegistry->snapshot()) {
            for (const char* f : fields) bdat.push_back(b.config.name + " " + f);
            for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch) {
                sipi.push_back(b.config.name + " I" + std::to_string(ch));
                sipt.push_back(b.config.name + " T" + std::to_string(ch));
            }
        }
        fOdbEqSettings->WSA((std::string("Names ") + BANK_NAME_BOARD_DATA).c_str(), bdat, 32);
        fOdbEqSettings->WSA((std::string("Names ") + BANK_NAME_SIPM_CURRENT).c_str(), sipi, 32);
        fOdbEqSettings->WSA((std::string("Names ") + BANK_NAME_SIPM_TEMP).c_str(), sipt, 32);
    }

    void CheckSettings() {
        int interval = fLastInterval;
        fOdbEqSettings->RI("Poll interval sec", &interval);
        if (interval >= 1 && interval != fLastInterval) {
            fLastInterval = interval;
            fPoller->set_poll_interval(interval);
            fPoller->poll_now();  // новый период — от следующего цикла
            fMfe->Msg(MINFO, "CheckSettings", "SlowControl: poll interval %d s", interval);
        }
        bool now = false;
        fOdbEqSettings->RB("Poll now", &now);
        if (now) {
            fOdbEqSettings->WB("Poll now", false);
            fPoller->poll_now();
        }
    }

    void Publish() {
        const auto boards = fRegistry->snapshot();
        std::map<BoardRegistry::Key, const BoardState*> by_key;
        for (const auto& b : boards) by_key[{b.branch_id, b.config.address}] = &b;

        // ---- событие: BDAT, SIPI, SIPT
        const size_t n = fOrder.size();
        std::vector<char> buf(1024 + n * (BDAT_VALUES_PER_BOARD + 2 * NUM_SIPM_CHANNELS) * sizeof(float) + 256);
        char* ev = buf.data();
        ComposeEvent(ev, buf.size());
        BkInit(ev, buf.size());
        float* d = (float*)BkOpen(ev, BANK_NAME_BOARD_DATA, TID_FLOAT);
        for (const auto& k : fOrder) {
            const BoardState& b = *by_key[k];
            const bool has = b.status == BoardStatus::ONLINE || b.status == BoardStatus::STALE;
            uint16_t analog = 0;
            for (size_t i = 0; i < NUM_ANALOG_BOARDS; ++i) analog |= b.health.analog_present[i] ? (1u << i) : 0;
            *d++ = (float)status_code(b.status);
            *d++ = has ? (float)b.data.update_counter : -1.0f;
            *d++ = has ? (float)b.data.cpu_temperature : -1.0f;
            *d++ = has ? (float)b.data.hv_status : -1.0f;
            *d++ = has ? (float)b.health.tripped_boards : -1.0f;
            *d++ = has ? (float)analog : -1.0f;
            *d++ = has ? (float)b.health.dac_mismatches.size() : -1.0f;
            *d++ = (float)b.reboots;
        }
        BkClose(ev, d);
        d = (float*)BkOpen(ev, BANK_NAME_SIPM_CURRENT, TID_FLOAT);
        for (const auto& k : fOrder) {
            const BoardState& b = *by_key[k];
            const bool has = b.status == BoardStatus::ONLINE || b.status == BoardStatus::STALE;
            for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch)
                *d++ = has && b.health.analog_present[analog_board_of_channel(ch)]
                           ? (float)b.data.sipm_channels[ch].leakage_current : -1.0f;
        }
        BkClose(ev, d);
        d = (float*)BkOpen(ev, BANK_NAME_SIPM_TEMP, TID_FLOAT);
        for (const auto& k : fOrder) {
            const BoardState& b = *by_key[k];
            const bool has = b.status == BoardStatus::ONLINE || b.status == BoardStatus::STALE;
            for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch)
                *d++ = has && b.health.temperature_valid[ch] ? (float)b.data.sipm_channels[ch].temperature : -1.0f;
        }
        BkClose(ev, d);
        EqSendEvent(ev);

        // ---- ODB: сводка и платы
        const auto sm = fRegistry->summary();
        const bool healthy = fRegistry->all_enabled_healthy();
        fOdbReadback->WI("Online", (int)sm.online);
        fOdbReadback->WI("Stale", (int)sm.stale);
        fOdbReadback->WI("Offline", (int)sm.offline);
        fOdbReadback->WI("Disabled", (int)sm.disabled);
        fOdbReadback->WI("With problems", (int)sm.with_problems);
        fOdbReadback->WI("Tripped", (int)sm.tripped);
        fOdbReadback->WB("All healthy", healthy);
        char ts[32];
        std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        fOdbReadback->WS("Last cycle", ts, 32);

        for (const auto& b : boards) {
            MVOdb* o = fOdbBoards->Chdir(odb_key(b.config.name).c_str(), true);
            o->WS("Status", to_string(b.status), 16);
            o->WI("Branch", b.branch_id);
            o->WI("Address", b.config.address);
            o->WI("Update counter", b.data.update_counter);
            o->WI("Analog boards", (int)b.health.analog_boards());
            o->WS("Problems", join(b.health.problems).c_str(), 256);
            o->WS("Last error", b.last_error.c_str(), 256);
            o->WI("Reboots", (int)b.reboots);
            o->WI("Failures", (int)b.failures);
            delete o;
            Alarms(b);
        }

        // ---- статус equipment на странице mhttpd
        const size_t enabled = sm.total - sm.disabled;
        char text[128];
        std::snprintf(text, sizeof(text), "%zu/%zu online", sm.online, enabled);
        std::string status = text;
        if (sm.offline) status += ", " + std::to_string(sm.offline) + " offline";
        if (sm.stale) status += ", " + std::to_string(sm.stale) + " stale";
        if (sm.tripped) status += ", " + std::to_string(sm.tripped) + " trip";
        const char* color = (sm.offline || sm.tripped) ? "redLight"
                            : (sm.stale || sm.with_problems) ? "yellowLight" : "greenLight";
        EqSetStatus(status.c_str(), color);
    }

    void SetAlarm(bool& state, bool now, const std::string& name, const std::string& msg) {
        if (now && !state) {
            // MIDAS пишет TALK-сообщение класса не чаще раза в «System message interval» (60 с),
            // поэтому каждое срабатывание дублируем в журнал сами
            fMfe->Msg(MERROR, "Alarms", "%s", msg.c_str());
            fMfe->TriggerAlarm(name.c_str(), msg.c_str(), "Alarm");
        }
        else if (!now && state) fMfe->ResetAlarm(name.c_str());
        state = now;
    }

    void Alarms(const BoardState& b) {
        AlarmState& a = fAlarms[{b.branch_id, b.config.address}];
        const std::string who = "SC " + b.config.name;
        const bool live = b.status == BoardStatus::ONLINE || b.status == BoardStatus::STALE;
        SetAlarm(a.offline, b.status == BoardStatus::OFFLINE, who + " offline",
                 who + ": board does not answer (" + b.last_error + ")");
        SetAlarm(a.stale, b.status == BoardStatus::STALE, who + " stale", who + ": update counter does not change");
        SetAlarm(a.trip, live && b.health.tripped_boards != 0, who + " trip",
                 who + ": HV trip, analog board mask " + std::to_string(b.health.tripped_boards));
        SetAlarm(a.dac, live && !b.health.dac_mismatches.empty(), who + " DAC",
                 who + ": " + std::to_string(b.health.dac_mismatches.size()) + " DAC channel(s) not applied");
        if (b.reboots > a.reboots) {
            if (a.status != BoardStatus::UNKNOWN)
                fMfe->Msg(MERROR, "Alarms", "SlowControl: board %s was rebooted (update counter restarted)",
                          b.config.name.c_str());
            a.reboots = b.reboots;
        }
        a.status = b.status;
    }
};

class FeSlowControl : public TMFrontend {
public:
    EqSlowControl* fEq = nullptr;

    FeSlowControl() {
        FeSetName("danss_sc_fe");
        fEq = new EqSlowControl(EQ_NAME, __FILE__);
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
        printf("SlowControl options (after --):\n");
        printf("  --config <file>   board_map.json (default config/board_map.json)\n");
    }

    TMFeResult HandleFrontendReady(const std::vector<std::string>& /*args*/) override {
        FeStartPeriodicThread();
        fMfe->StartRpcThread();
        return TMFeOk();
    }

    void HandleFrontendExit() override {
        fMfe->Msg(MINFO, "HandleFrontendExit", "SlowControl frontend stopped");
    }
};

int main(int argc, char* argv[]) {
    FeSlowControl fe;
    return fe.FeMain(argc, argv);
}
