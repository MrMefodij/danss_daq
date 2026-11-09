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
//   Settings/Exclusions file    — JSON с ручными исключениями (system.exclusions_file)
//   Settings/Poll interval sec  — период опроса; меняется на лету
//   Settings/Poll now           — y: опросить сейчас (сбрасывается сам)
//   Settings/Names BDAT|SIPI|SIPT — подписи для истории
//   Settings/Boards/<имя>/      — ручные исключения: Exclude analog board[3], Exclude counter[12],
//                                 Exclusion reason. При старте берутся из файла исключений (файл
//                                 главнее ODB), каждое изменение пишется в журнал и сразу в файл.
//   Readback/                   — сводка: Online, Stale, Offline, Disabled, With problems,
//                                 With missing, Tripped, All healthy, Missing policy, Last cycle
//   Boards/<имя>/               — по плате: Status, Branch, Address, Update counter, Analog boards seen,
//                                 Counters seen, Missing, Excluded, Problems, Last error, Reboots,
//                                 Failures, Current uA[12], Temperature C[12]
//   Variables/BDAT              — плата × 9: status (0 unknown 1 online 2 stale 3 offline 4 disabled),
//                                 update counter, CPU temp (raw), HV status, tripped mask,
//                                 analog boards seen (mask), counters seen (mask), DAC mismatches, reboots;
//                                 -1 — нет данных
//   Variables/SIPI              — плата × 12: токи утечки SiPM, мкА (ADC × 6.6 / 1024)
//   Variables/SIPT              — плата × 12: температуры SiPM, °C (int16 / 16)
//                                 NaN — нет данных (плата не отвечает / нет доступа к термометру)
//
// Алармы (класс Alarm): «SC <плата> offline», «stale», «trip», «DAC», «missing» (неисключённая
// аналоговая плата или счётчик не видны; при system.missing_component = "warning" — только
// сообщение). Перезагрузка платы — сообщение MIDAS, не аларм.
// «All healthy» — кандидат в источник интерлока HV для danss_psu_fe (условия — решение группы).
// JRPC: "poll_now".

#undef NDEBUG  // MIDAS требует assert()

#include "tmfe.h"
#include "midas.h"

#include "common/config_loader.h"
#include "common/constants.h"
#include "common/logger.h"
#include "rs485/board_registry.h"
#include "slowcontrol/exclusions_file.h"
#include "slowcontrol/periodic_poller.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace danss;
using namespace danss::constants;
using collector::mask_to_list;

namespace {

const char* EQ_NAME = EQUIPMENT_NAME_SLOW_CONTROL;
const float NO_DATA = std::numeric_limits<float>::quiet_NaN();

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

std::string now_string() {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return ts;
}

std::vector<bool> mask_to_bools(uint16_t m, size_t n) {
    std::vector<bool> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (m >> i) & 1;
    return v;
}

uint16_t bools_to_mask(const std::vector<bool>& v, size_t n) {
    uint16_t m = 0;
    for (size_t i = 0; i < n && i < v.size(); ++i) m |= v[i] ? (1u << i) : 0;
    return m;
}

std::string describe(const collector::Exclusions& e) {
    std::string s;
    if (e.analog_boards) s += "analog boards " + mask_to_list(e.analog_boards, NUM_ANALOG_BOARDS);
    if (e.counters) s += std::string(s.empty() ? "" : "; ") + "counters " + mask_to_list(e.counters, NUM_SIPM_CHANNELS);
    return s.empty() ? "nothing" : s;
}

std::string describe_missing(const collector::BoardHealth& h) {
    std::string s;
    auto add = [&](const std::string& x) { s += (s.empty() ? "" : "; ") + x; };
    if (h.missing_analog) add("analog boards " + mask_to_list(h.missing_analog, NUM_ANALOG_BOARDS));
    if (h.missing_counters) add("counters " + mask_to_list(h.missing_counters, NUM_SIPM_CHANNELS));
    if (h.temperature_errors) add("temperature unreadable " + mask_to_list(h.temperature_errors, NUM_SIPM_CHANNELS));
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
        fOdbEqSettings->WS("Exclusions file", fConfig.exclusions_file.c_str(), 256);
        int interval = static_cast<int>(fConfig.poll_interval_sec);
        fOdbEqSettings->RI("Poll interval sec", &interval, true);  // создать, если нет; иначе взять из ODB
        if (interval < 1) interval = 1;
        fLastInterval = interval;
        fOdbEqSettings->WB("Poll now", false);
        WriteNames();

        fOdbReadback = fOdbEq->Chdir("Readback", true);
        fOdbBoards = fOdbEq->Chdir("Boards", true);
        fOdbSettingsBoards = fOdbEqSettings->Chdir("Boards", true);

        TMFeResult r = InitExclusions();
        if (r.error_flag) return r;

        fPoller = std::make_unique<PeriodicPoller>(fConfig, *fRegistry);
        fPoller->set_poll_interval(interval);
        fPoller->set_cycle_callback([this](uint8_t) { fCycleDone = true; });
        fPoller->start();

        size_t enabled = 0;
        for (const auto& br : fConfig.branches)
            for (const auto& b : br.boards) enabled += b.enabled;
        fMfe->Msg(MINFO, "HandleInit", "SlowControl: %zu branch(es), %zu board(s) (%zu enabled), poll every %d s, "
                  "missing component -> %s", fConfig.branches.size(), fOrder.size(), enabled, interval,
                  fConfig.missing_is_alarm ? "alarm" : "warning");
        return TMFeOk();
    }

    void HandlePeriodic() override {
        if (!fPoller) return;
        CheckSettings();
        CheckExclusions();
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
    MVOdb* fOdbSettingsBoards = nullptr;
    int fLastInterval = 0;

    // Ручные исключения: имя платы -> текущее значение и каталог ODB с его настройками
    ExclusionMap fExcl;
    std::map<std::string, MVOdb*> fOdbExcl;

    struct AlarmState {
        bool offline = false, stale = false, trip = false, dac = false, missing = false;
        std::string missing_text;
        uint64_t reboots = 0;
        BoardStatus status = BoardStatus::UNKNOWN;
    };
    std::map<BoardRegistry::Key, AlarmState> fAlarms;

    // ---- ручные исключения -----------------------------------------------------

    TMFeResult InitExclusions() {
        bool exists = false;
        ExclusionMap file;
        if (!fConfig.exclusions_file.empty()) {
            try {
                file = load_exclusions(fConfig.exclusions_file, exists);
            } catch (const ConfigError& e) {
                EqSetStatus("Invalid exclusions file", "redLight");
                return TMFeErrorMessage(std::string("Invalid exclusions file: ") + e.what());
            }
        } else {
            fMfe->Msg(MINFO, "InitExclusions", "SlowControl: system.exclusions_file is not set in %s — "
                      "exclusions live only in ODB", fConfigFile.c_str());
        }

        for (const auto& b : fRegistry->snapshot()) {
            const std::string& name = b.config.name;
            MVOdb* o = fOdbSettingsBoards->Chdir(odb_key(name).c_str(), true);
            fOdbExcl[name] = o;
            BoardExclusion e;
            if (exists) {
                auto it = file.find(name);
                if (it != file.end()) e = it->second;  // файл главнее ODB; нет в файле — ничего не исключено
            } else {
                e = ReadOdbExclusion(o);  // файла ещё нет — берём то, что есть в ODB
            }
            WriteOdbExclusion(o, e);
            fExcl[name] = e;
            fRegistry->set_exclusions(b.branch_id, b.config.address, e.masks);
            if (!e.empty())
                fMfe->Msg(MINFO, "InitExclusions", "SlowControl: %s: excluded %s (reason: %s; since %s)", name.c_str(),
                          describe(e.masks).c_str(), e.reason.empty() ? "-" : e.reason.c_str(),
                          e.changed.empty() ? "-" : e.changed.c_str());
        }
        for (const auto& [name, e] : file)
            if (!fExcl.count(name))
                fMfe->Msg(MERROR, "InitExclusions", "SlowControl: %s: board \"%s\" is not in %s — ignored",
                          fConfig.exclusions_file.c_str(), name.c_str(), fConfigFile.c_str());
        if (!fConfig.exclusions_file.empty() && !exists) SaveExclusions();
        return TMFeOk();
    }

    BoardExclusion ReadOdbExclusion(MVOdb* o) {
        BoardExclusion e;
        std::vector<bool> a(NUM_ANALOG_BOARDS, false), c(NUM_SIPM_CHANNELS, false);
        o->RBA("Exclude analog board", &a, true, NUM_ANALOG_BOARDS);
        o->RBA("Exclude counter", &c, true, NUM_SIPM_CHANNELS);
        o->RS("Exclusion reason", &e.reason, true, 256);
        e.masks.analog_boards = bools_to_mask(a, NUM_ANALOG_BOARDS);
        e.masks.counters = bools_to_mask(c, NUM_SIPM_CHANNELS);
        return e;
    }

    void WriteOdbExclusion(MVOdb* o, const BoardExclusion& e) {
        o->WBA("Exclude analog board", mask_to_bools(e.masks.analog_boards, NUM_ANALOG_BOARDS));
        o->WBA("Exclude counter", mask_to_bools(e.masks.counters, NUM_SIPM_CHANNELS));
        o->WS("Exclusion reason", e.reason.c_str(), 256);
    }

    void SaveExclusions() {
        if (fConfig.exclusions_file.empty()) return;
        try {
            save_exclusions(fConfig.exclusions_file, fExcl);
        } catch (const std::exception& e) {
            fMfe->Msg(MERROR, "SaveExclusions", "SlowControl: cannot save exclusions: %s", e.what());
            fMfe->TriggerAlarm("SC exclusions file", (std::string("SlowControl: cannot save exclusions: ") + e.what()).c_str(),
                               "Alarm");
            return;
        }
        fMfe->ResetAlarm("SC exclusions file");
    }

    /// Оператор поменял исключения в ODB: журнал, файл, реестр
    void CheckExclusions() {
        bool changed_any = false, masks_changed = false;
        for (const auto& [name, o] : fOdbExcl) {
            BoardExclusion now = ReadOdbExclusion(o);
            BoardExclusion& cur = fExcl[name];
            const bool masks = now.masks.analog_boards != cur.masks.analog_boards || now.masks.counters != cur.masks.counters;
            if (!masks && now.reason == cur.reason) continue;
            const std::string was = describe(cur.masks);
            now.changed = masks ? now_string() : cur.changed;
            cur = now;
            changed_any = true;
            if (masks) {
                masks_changed = true;
                auto b = fRegistry->find(name);
                if (b) fRegistry->set_exclusions(b->branch_id, b->config.address, cur.masks);
                fMfe->Msg(MINFO, "CheckExclusions", "SlowControl: %s: operator excluded %s (was: %s); reason: %s",
                          name.c_str(), describe(cur.masks).c_str(), was.c_str(),
                          cur.reason.empty() ? "NOT GIVEN" : cur.reason.c_str());
            } else {
                fMfe->Msg(MINFO, "CheckExclusions", "SlowControl: %s: exclusion reason: %s", name.c_str(),
                          cur.reason.c_str());
            }
        }
        if (changed_any) SaveExclusions();
        if (masks_changed) fPoller->poll_now();  // проверка с новыми исключениями — сразу
    }

    // ---- настройки опроса ------------------------------------------------------

    void WriteNames() {
        std::vector<std::string> bdat, sipi, sipt;
        const char* fields[BDAT_VALUES_PER_BOARD] = {"status", "counter", "CPU T raw", "HV st", "trip",
                                                     "analog", "counters", "DAC!", "reboots"};
        for (const auto& b : fRegistry->snapshot()) {
            for (const char* f : fields) bdat.push_back(b.config.name + " " + f);
            for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch) {
                sipi.push_back(b.config.name + " I" + std::to_string(ch) + " uA");
                sipt.push_back(b.config.name + " T" + std::to_string(ch) + " C");
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

    // ---- публикация ------------------------------------------------------------

    void Publish() {
        const auto boards = fRegistry->snapshot();
        std::map<BoardRegistry::Key, const BoardState*> by_key;
        for (const auto& b : boards) by_key[{b.branch_id, b.config.address}] = &b;
        auto live = [](const BoardState& b) { return b.status == BoardStatus::ONLINE || b.status == BoardStatus::STALE; };

        // ---- событие: BDAT, SIPI, SIPT
        const size_t n = fOrder.size();
        std::vector<char> buf(1024 + n * (BDAT_VALUES_PER_BOARD + 2 * NUM_SIPM_CHANNELS) * sizeof(float) + 256);
        char* ev = buf.data();
        ComposeEvent(ev, buf.size());
        BkInit(ev, buf.size());
        float* d = (float*)BkOpen(ev, BANK_NAME_BOARD_DATA, TID_FLOAT);
        for (const auto& k : fOrder) {
            const BoardState& b = *by_key[k];
            const bool has = live(b);
            *d++ = (float)status_code(b.status);
            *d++ = has ? (float)b.data.update_counter : -1.0f;
            *d++ = has ? (float)b.data.cpu_temperature : -1.0f;
            *d++ = has ? (float)b.data.hv_status : -1.0f;
            *d++ = has ? (float)b.health.tripped_boards : -1.0f;
            *d++ = has ? (float)b.health.analog_mask() : -1.0f;
            *d++ = has ? (float)b.health.counter_mask() : -1.0f;
            *d++ = has ? (float)b.health.dac_mismatches.size() : -1.0f;
            *d++ = (float)b.reboots;
        }
        BkClose(ev, d);
        d = (float*)BkOpen(ev, BANK_NAME_SIPM_CURRENT, TID_FLOAT);
        for (const auto& k : fOrder) {
            const BoardState& b = *by_key[k];
            for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch)
                *d++ = live(b) ? (float)sipm_current_ua(b.data.sipm_channels[ch].leakage_current) : NO_DATA;
        }
        BkClose(ev, d);
        d = (float*)BkOpen(ev, BANK_NAME_SIPM_TEMP, TID_FLOAT);
        for (const auto& k : fOrder) {
            const BoardState& b = *by_key[k];
            for (size_t ch = 0; ch < NUM_SIPM_CHANNELS; ++ch)
                *d++ = live(b) && b.health.temperature_valid[ch]
                           ? (float)sipm_temperature_c(b.data.sipm_channels[ch].temperature) : NO_DATA;
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
        fOdbReadback->WI("With missing", (int)sm.with_missing);
        fOdbReadback->WI("Tripped", (int)sm.tripped);
        fOdbReadback->WB("All healthy", healthy);
        fOdbReadback->WS("Missing policy", fConfig.missing_is_alarm ? "alarm" : "warning", 16);
        fOdbReadback->WS("Last cycle", now_string().c_str(), 32);

        for (const auto& b : boards) {
            const bool has = live(b);
            MVOdb* o = fOdbBoards->Chdir(odb_key(b.config.name).c_str(), true);
            o->WS("Status", to_string(b.status), 16);
            o->WI("Branch", b.branch_id);
            o->WI("Address", b.config.address);
            o->WI("Update counter", b.data.update_counter);
            o->WS("Analog boards seen", has ? mask_to_list(b.health.analog_mask(), NUM_ANALOG_BOARDS).c_str() : "", 32);
            o->WS("Counters seen", has ? mask_to_list(b.health.counter_mask(), NUM_SIPM_CHANNELS).c_str() : "", 64);
            o->WS("Missing", has ? describe_missing(b.health).c_str() : "", 256);
            o->WS("Excluded", b.excluded.analog_boards || b.excluded.counters ? describe(b.excluded).c_str() : "", 256);
            o->WS("Problems", join(b.health.problems).c_str(), 256);
            o->WS("Last error", b.last_error.c_str(), 256);
            o->WI("Reboots", (int)b.reboots);
            o->WI("Failures", (int)b.failures);
            std::vector<double> cur(NUM_SIPM_CHANNELS, NO_DATA), temp(NUM_SIPM_CHANNELS, NO_DATA);
            for (size_t ch = 0; ch < NUM_SIPM_CHANNELS && has; ++ch) {
                cur[ch] = sipm_current_ua(b.data.sipm_channels[ch].leakage_current);
                if (b.health.temperature_valid[ch]) temp[ch] = sipm_temperature_c(b.data.sipm_channels[ch].temperature);
            }
            o->WDA("Current uA", cur);
            o->WDA("Temperature C", temp);
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
        if (sm.with_missing) status += ", " + std::to_string(sm.with_missing) + " incomplete";
        const bool red = sm.offline || sm.tripped || (sm.with_missing && fConfig.missing_is_alarm);
        const char* color = red ? "redLight" : (sm.stale || sm.with_problems) ? "yellowLight" : "greenLight";
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

        // Состав: что ожидается (не исключено вручную), но не видно
        if (live) {
            const std::string missing = describe_missing(b.health);
            if (fConfig.missing_is_alarm) {
                if (!missing.empty() && a.missing && missing != a.missing_text)
                    fMfe->Msg(MERROR, "Alarms", "%s: not seen now: %s", who.c_str(), missing.c_str());
                SetAlarm(a.missing, !missing.empty(), who + " missing", who + ": not seen: " + missing);
            } else if (missing != a.missing_text) {
                if (missing.empty()) fMfe->Msg(MINFO, "Alarms", "%s: all expected components seen", who.c_str());
                else fMfe->Msg(MINFO, "Alarms", "%s: not seen: %s", who.c_str(), missing.c_str());
            }
            a.missing_text = missing;
        }

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
