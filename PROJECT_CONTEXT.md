# DANSS Slow Control & DAQ — контекст проекта

> Этот файл предназначен для переноса контекста в новый чат.
> Вставьте его целиком (или приложите файлом) в начало нового диалога.

---

## 0. Как использовать этот документ

Начните новый чат фразой вроде:

> «Продолжаем работу над проектом DANSS Slow Control. Ниже — полный контекст проекта.
> Текущая задача: <опишите задачу>.»

и приложите этот файл + архив `danss-slow-control.tar.gz`.

---

## 1. О проекте

**Название:** DANSS Slow Control & DAQ Server
**Назначение:** система медленного контроля и сбора данных для детектора DANSS на базе фреймворка MIDAS.
**Язык:** C++17
**Платформа:** Linux (Ubuntu 22.04/24.04, GCC 11+)

### Требования

| Параметр | Значение |
|---|---|
| Веток RS-485 | 4–8 (USB-RS485 адаптеры `/dev/ttyUSB0..7`) — в проектных документах; **на реальном тестовом стенде ДАНСС-2 сейчас используется `/dev/ttyACM3`** (см. §13) |
| Плат всего | ~240 (~30 на ветку) в целевой конфигурации; **на тестовом стенде сейчас реально подключено 2 платы (адреса 1 и 2)** |
| Период опроса | 5 минут |
| Что опрашивается | токи утечки SiPM, температуры, состояния, срабатывания авто-отключения |
| EEPROM | платы хранят настройки; при старте считать и сверить; раз в сутки контролировать расхождения |
| Синхронизация | жёсткая синхронизация DAQ/SlowControl при переключении Calibration ↔ Physics Run |

### Технологический стек

- **MIDAS 2.1** (`mfe.cxx` / `mfe.h` framework), у пользователя собран в `~/work/danss/packages/midas`
- **Boost.Asio 1.83** — асинхронный I/O, serial port
- **nlohmann/json 3.11.2** — подтягивается через CMake FetchContent
- **CMake 3.12+**

---

## 2. Протокол — Modbus RTU over RS-485

**Параметры порта:** 19200 бод (может меняться), 8 данных, без чётности, 1 стоп-бит, без flow control.

> Подтверждено разработчиком прошивки плат (Игорь), сентябрь 2026: скорость пока жёстко задана 19200 в прошивке, в дальнейшем планируется регистр для выбора скорости.

### Функции

| Код | Функция |
|---|---|
| `0x03` | Read Holding Registers |
| `0x04` | Read Input Registers |
| `0x06` | Write Single Register |
| `0x10` | Write Multiple Registers |

### Карта регистров — версия 2 (октябрь 2026)

Полная карта, scratchpad и открытые вопросы: [doc/MODBUS_MAP_v2.md](doc/MODBUS_MAP_v2.md).
На одну процессорную плату — 3 аналоговые платы, 12 каналов SiPM.

**Input-регистры (read-only)**

| Адрес | Содержимое |
|---|---|
| 0 | Порядковый номер обновления (растёт в конце каждого цикла) |
| 1 | Статус HV: `0ttt eeee eeee eeee` — trip по аналоговой плате, HV по каналу |
| 2 | `0000 0rrr 0000 0aaa` — разрешение trip и усиление по аналоговой плате |
| 3–14 | Токи утечки SiPM (12 каналов) |
| 15–26 | Температуры SiPM (12 каналов) |
| 27 | Температура CPU |
| 100–147 | ID термометров 1-Wire (12 × 4 регистра) |
| 148–149 | CPU ID |
| 150–245 | Обратное чтение DAC |
| 246 | Смещение датчика температуры CPU |
| 250–761 | Обратное чтение scratchpad (512 слов, сумма = 0xAA55) |

**Holding-регистры (read/write)**

| Адрес | Содержимое |
|---|---|
| 0–95 | Настройки DAC (96 каналов) |
| 96 | Counter ON/OFF (SiPM HV enable), бит на канал |
| 97 | Усиление и разрешение trip, бит на аналоговую плату |
| 98 | Global CSR: команды 101 загрузить / 103 стереть / 105 сохранить scratchpad; статус 0 / 10 / 20 |
| 99 | Задержка между циклами |
| 100 | Адрес устройства (только с заводского 247) |

### Тайминги (расчёт)

- Одна транзакция ≈ 35–85 мс на 19200 бод
- Одна плата (2 транзакции: input + holding) ≈ 85 мс
- Ветка (30 плат, последовательно) ≈ 2.8 с
- Все 8 веток параллельно ≈ < 3 с
- **Запас по времени: ~100× относительно 5-минутного цикла**

Чтение EEPROM (48 + 2 регистра) ≈ 95 мс на плату, все 240 плат ≈ < 3 с.

---

## 3. Архитектура

```
┌─────────────────────────────────────────┐
│         MIDAS DAQ (ODB, mlogger, mhttpd)│
└───────────────┬─────────────────────────┘
                │
     ┌──────────▼───────────┐
     │  danss_frontend      │
     │  ├ SlowControl Equip │  ← read_event() каждые 5 мин
     │  └ RunSync Equipment │  ← переходы режимов
     └──────────┬───────────┘
                │
     ┌──────────▼───────────┐
     │  RS485BranchManager  │  ← 4–8 потоков, по одному на ветку
     └──────────┬───────────┘
                │
     ┌──────────▼───────────┐
     │  BoardRegistry       │  ← 240 плат, состояния, EEPROM-эталоны
     └──────────┬───────────┘
                │
   ┌────────────┼────────────┬─────────┐
┌──▼──┐      ┌──▼──┐      ┌──▼──┐   ┌──▼──┐
│ttyUSB0│    │ttyUSB1│    │ttyUSB2│  │ ... │
│ModbusRTU│  │ModbusRTU│  │ModbusRTU│ │     │
└──┬──┘      └──┬──┘      └──┬──┘   └─────┘
   │            │            │
  ~30 плат    ~30 плат     ~30 плат
```

### Модель многопоточности

- Главный поток координирует циклы опроса
- Один рабочий поток на ветку RS-485 (свой `ModbusRTU`, свой `io_context`)
- Общий буфер данных под read-write lock
- Очередь алармов под mutex

### State Machine (режимы)

```
STOPPED ⇄ PHYSICS_RUN ⇄ CALIBRATION_RUN
   └──────── MAINTENANCE
```

Правила переходов:
1. **STOPPED → PHYSICS** — проверить, что все платы онлайн, загрузить DAC, включить HV
2. **PHYSICS → CALIBRATION** — дождаться паузы, переставить DAC на калибровочные значения, включить LED-пульсер
3. **CALIBRATION → PHYSICS** — восстановить физические DAC, выключить пульсер
4. **ANY → STOPPED** — выключить HV, сохранить состояние, закрыть связь

---

## 4. Структура проекта

```
danss-slow-control/
├── CMakeLists.txt                    ✅ готово
├── README.md                         ✅
├── QUICKSTART.md                     ✅
├── TODO.md                           ✅ план разработки
├── PROJECT_SUMMARY.md                ✅
├── BUGFIXES.md                       ✅ история исправлений
├── PROJECT_CONTEXT.md                ← этот файл
├── .gitignore                        ✅
│
├── config/
│   ├── board_map.json                ✅ пример (5 плат)
│   ├── eeprom_reference.json         ✅ пример (5 плат)
│   └── generate_full_config.py       ✅ генератор на 240 плат
│
├── doc/
│   ├── BUILD.md                      ✅
│   ├── ARCHITECTURE.md               ✅
│   ├── MODBUS_SCANNER.md             ✅
│   ├── SCANNER_CHEATSHEET.md         ✅
│   └── SCANNER_OUTPUT_EXAMPLES.md    ✅
│
├── include/
│   ├── common/
│   │   ├── types.h                   ✅ BoardData, SystemConfig, BranchConfig, RunMode, Alarm
│   │   ├── constants.h               ✅ карта регистров, коды функций, пороги
│   │   ├── logger.h                  ✅
│   │   └── config_loader.h           ⚠️ заглушка (объявления есть, реализации нет)
│   └── modbus/
│       └── modbus_rtu.h              ✅
│
├── src/
│   ├── frontend/
│   │   ├── main.cpp                  ✅ CLI, --test-modbus, --standalone
│   │   └── equipment.cpp             ⚠️ заглушка
│   ├── modbus/
│   │   ├── modbus_rtu.cpp            ✅ РАБОТАЕТ
│   │   └── modbus_crc.cpp            ✅ CRC16 с lookup-таблицей
│   ├── utils/
│   │   ├── logger.cpp                ✅
│   │   ├── config_loader.cpp         ⚠️ заглушка (возвращает дефолты)
│   │   └── modbus_scanner.cpp        ✅ отдельная утилита
│   ├── rs485/
│   │   ├── rs485_manager.cpp         ⏳ TODO (пустой)
│   │   └── board_registry.cpp        ⏳ TODO (пустой)
│   ├── eeprom/
│   │   ├── eeprom_manager.cpp        ⏳ TODO (пустой)
│   │   └── eeprom_validator.cpp      ⏳ TODO (пустой)
│   ├── slowcontrol/
│   │   ├── periodic_poller.cpp       ⏳ TODO (пустой)
│   │   └── data_collector.cpp        ⏳ TODO (пустой)
│   ├── sync/
│   │   └── state_machine.cpp         ⏳ TODO (пустой)
│   └── power/
│       ├── owon_psu.cpp              ✅ готово — SCPI/TCP клиент OWON ODP6033
│       ├── power_controller.cpp      ✅ готово — политика (OVP/OCP, ramp, интерлок)
│       └── power_equipment.cpp       ⚠️ заглушка (MIDAS Equipment, Фаза 4)
│
└── tests/                            ⏳ TODO (пусто)
```

Соответствующие заголовки: `include/power/owon_psu.h`, `include/power/power_controller.h`.
Пример конфига: `config/power_supply.json`.

**Цели сборки:** статическая библиотека `danss_core`, исполняемые `danss_frontend` и `modbus_scanner`.

---

## 5. Текущее состояние

### ✅ Фаза 1 — готово

- Структура проекта, CMake (MIDAS + Boost + FetchContent nlohmann/json)
- **ModbusRTU** — полностью рабочий класс: все 4 функции, CRC16, таймауты, статистика, thread-safe
- Типы данных и константы (вся карта регистров)
- Logger (уровни, файл, макросы `LOG_INFO` и т.д.)
- **modbus_scanner** — утилита сканирования шины
- Примеры JSON-конфигов + генератор на 240 плат
- Документация (~40 страниц)

### 🚧 Фаза 2 — следующая (Modbus-часть не начата; PSU-часть — см. ниже)

- `ConfigLoader` — реальный парсинг JSON через nlohmann/json (сейчас возвращает дефолты, включая новый `load_power_config()`)
- `BoardRegistry` — загрузка 240 плат, поиск по имени/адресу/ветке, трекинг состояния online/offline/error
- `RS485BranchManager` — multi-threading, по потоку на ветку, retry, статистика
- `PeriodicPoller` — цикл 5 минут, параллельный опрос веток
- `DataCollector` — парсинг сырых регистров в `BoardData`, калибровки, детект аномалий

**Управление блоком питания OWON ODP6033 — отдельный подтрек Фазы 2, код готов** (см. §15):
`OwonPSU` (SCPI/TCP-клиент) и `PowerSupplyController` (политика: OVP/OCP, плавный
разгон HV, интерлок, аварийное отключение) написаны и синтаксически проверены.
Не сделано: реальный парсинг `power_supply.json` в `ConfigLoader`, проверка на
реальном блоке на стенде, интеграция интерлока с `BoardRegistry` (которого еще нет),
MIDAS-обвязка (`power_equipment.cpp`, Фаза 4).

### 📋 Фаза 3 — потом

- `EEPROMManager` / `EEPROMValidator` — суточная сверка, отчёты о расхождениях, авто-восстановление
- MIDAS-интеграция: `SlowControlEquipment`, `RunSyncEquipment`, банки (`BDAT`, `STAT`, `ALRM`), ODB
- `RunStateMachine` — переходы Calibration ↔ Physics
- Алармы MIDAS (заготовка под Telegram)

### 📋 Фаза 4+ — error recovery, логирование, Telegram, unit-тесты, оптимизация

---

## 6. Ключевые технические решения и подводные камни

### ⚠️ Boost.Asio serial_port НЕ имеет метода `available()`

Это метод сокетов. Для serial port нужен другой подход. **Текущая реализация `transact()` использует POSIX `select()`** на дескрипторе, полученном через `serial_port_->native_handle()`:

```cpp
#ifdef __linux__
#include <sys/select.h>
#include <unistd.h>
#endif

int fd = serial_port_->native_handle();
fd_set read_fds;
FD_ZERO(&read_fds);
FD_SET(fd, &read_fds);

struct timeval tv;
tv.tv_sec  = remaining_ms / 1000;
tv.tv_usec = (remaining_ms % 1000) * 1000;

int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
if (ret > 0) {
    // данные есть — читаем побайтно, затем ждём T3.5 и дочитываем хвост
} else if (ret == 0) {
    // таймаут
}
```

### ⚠️ Нельзя использовать блокирующий `boost::asio::read` с `transfer_at_least`

Первая версия зависала намертво при отсутствии ответа платы (при сканировании несуществующих адресов). Именно поэтому и появился `select()`.

### Определение границы фрейма Modbus RTU

После получения минимального фрейма (4 байта) ждём `T3.5` (≈1.75 мс на 19200 бод), затем проверяем, не пришло ли ещё данных. Если нет — фрейм считается завершённым.

### CRC16

Modbus-полином `0xA001`, реализован через lookup-таблицу на 256 записей в `modbus_crc.cpp`. Передаётся **LSB первым**.

### Заголовки, которых легко не хватает

- `constants.h` → нужен `#include <cstddef>` (для `size_t`)
- `config_loader.h` → нужен `#include <map>`
- `main.cpp` → нужны `#include <thread>` и `#include <chrono>`

---

## 7. Сборка и запуск

### Сборка

```bash
export MIDASSYS=/home/amefodev/work/danss/packages/midas
export MIDAS_EXPTAB=/etc/exptab

mkdir build && cd build
cmake ..
make -j$(nproc)
```

Выход: `build/bin/danss_frontend`, `build/bin/modbus_scanner`, `build/lib/libdanss_core.a`.

### modbus_scanner

```bash
# базовое сканирование
./bin/modbus_scanner /dev/ttyUSB0

# с показом текущего адреса в прогресс-баре
./bin/modbus_scanner --show-addresses /dev/ttyUSB0

# диапазон + подробный вывод
./bin/modbus_scanner -v -s 1 -e 30 /dev/ttyUSB0

# все ветки
for i in {0..7}; do ./bin/modbus_scanner /dev/ttyUSB$i > branch${i}.txt; done

# на реальном тестовом стенде ДАНСС-2 (см. §13): устройство /dev/ttyACM3, платы 1 и 2
./bin/modbus_scanner -s 1 -e 2 /dev/ttyACM3
```

Опции: `-s/--start`, `-e/--end`, `-t/--timeout` (мс), `-v/--verbose`, `-q/--quick`, `--show-addresses`, `--delay` (мс).

На выходе — таблица найденных плат + **готовый JSON-фрагмент для `board_map.json`**.

### danss_frontend

```bash
# тест платы по адресу 1 (по умолчанию)
./bin/danss_frontend --test-modbus /dev/ttyUSB0 19200

# тест платы по конкретному адресу
./bin/danss_frontend --test-modbus /dev/ttyUSB0 19200 5

# режим без MIDAS
./bin/danss_frontend --standalone -v

# полноценный MIDAS-режим (пока НЕ реализован)
./bin/danss_frontend -e DANSS
```

### Генерация конфигурации на 240 плат

```bash
cd config
python3 generate_full_config.py            > board_map_full.json
python3 generate_full_config.py --eeprom   > eeprom_reference_full.json
```

---

## 8. Формат конфигурации

### `board_map.json`

```json
{
  "system": {
    "poll_interval_sec": 300,
    "eeprom_check_interval_sec": 86400,
    "modbus_timeout_ms": 1000,
    "response_timeout_ms": 500,
    "max_retries": 3,
    "retry_delay_ms": 100
  },
  "branches": [
    {
      "id": 0,
      "device": "/dev/ttyUSB0",
      "baud_rate": 19200,
      "data_bits": 8,
      "parity": "N",
      "stop_bits": 1,
      "boards": [
        { "address": 1, "name": "PMT-01-01", "enabled": true }
      ]
    }
  ]
}
```

### `eeprom_reference.json`

```json
{
  "description": "EEPROM reference data for board validation",
  "last_updated": "2025-01-18",
  "boards": [
    {
      "name": "PMT-01-01",
      "modbus_address": 1,
      "branch_id": 0,
      "thermometer_ids": ["28FF641E8316055A", "..."],
      "cpu_id": "ABC123DEF456"
    }
  ]
}
```

---

## 9. Ключевые структуры данных (`include/common/types.h`)

```cpp
namespace danss {

struct SiPMData {
    uint16_t leakage_current;      // input 3–14
    uint16_t temperature;          // input 15–26
    std::string thermometer_id;    // 1-Wire ID из EEPROM (input 100–147)
};

struct BoardData {                 // карта регистров v2, doc/MODBUS_MAP_v2.md
    uint8_t  modbus_address;
    std::string board_name;
    uint16_t update_counter;       // input 0
    uint16_t hv_status;            // input 1: trip по аналоговой плате + HV по каналу
    uint16_t gain_trip_status;     // input 2
    std::array<SiPMData, 12> sipm_channels;
    uint16_t cpu_temperature;      // input 27
    uint16_t cpu_temp_offset;      // input 246
    std::string cpu_id;            // input 148–149
    std::array<uint16_t, 96> dac_readback;  // input 150–245
    std::array<uint16_t, 96> dac_settings;  // holding 0–95
    uint16_t hv_enable;            // holding 96
    uint16_t gain_trip_enable;     // holding 97
    uint16_t global_csr;           // holding 98 (статус команды)
    uint16_t cycle_delay;          // holding 99
    uint16_t device_address;       // holding 100
    uint64_t timestamp_ms;
    bool data_valid;
    bool data_fresh;               // input 0 изменился с прошлого опроса
    std::string error_message;
    // + hv_channel_enabled(), analog_board_tripped(), any_trip(), dac_mismatches()
};

struct ScratchpadImage { std::array<uint16_t, 512> words;   // input 250–761
                         bool checksum_ok() const; /* сумма = 0xAA55 */ };

struct BoardConfig  { uint8_t address; std::string name; bool enabled;
                      std::vector<std::string> thermometer_ids; std::string cpu_id; };
struct BranchConfig { uint8_t id; std::string device; uint32_t baud_rate;
                      uint8_t data_bits; char parity; uint8_t stop_bits;
                      std::vector<BoardConfig> boards; };
struct SystemConfig { std::vector<BranchConfig> branches;
                      uint32_t poll_interval_sec, eeprom_check_interval_sec;
                      uint32_t modbus_timeout_ms, response_timeout_ms;
                      uint8_t max_retries; uint32_t retry_delay_ms; };

enum class RunMode : uint8_t { STOPPED, PHYSICS_RUN, CALIBRATION_RUN, MAINTENANCE };
enum class AlarmSeverity : uint8_t { INFO, WARNING, ERROR, CRITICAL };

struct BranchStats { uint64_t total_transactions, successful_reads, failed_reads,
                     timeouts, crc_errors; double avg_response_time_ms; };
}
```

## Интерфейс ModbusRTU (`include/modbus/modbus_rtu.h`)

```cpp
class ModbusRTU {
public:
    ModbusRTU(const std::string& device, uint32_t baud_rate = 19200);
    bool open();  void close();  bool is_open() const;
    void set_timeout(uint32_t timeout_ms);

    bool read_input_registers  (uint8_t slave, uint16_t start, uint16_t count,
                                std::vector<uint16_t>& out);
    bool read_holding_registers(uint8_t slave, uint16_t start, uint16_t count,
                                std::vector<uint16_t>& out);
    bool write_single_register (uint8_t slave, uint16_t reg, uint16_t value);
    bool write_multiple_registers(uint8_t slave, uint16_t start,
                                  const std::vector<uint16_t>& values);

    struct Statistics { uint64_t total_requests, successful_responses, timeouts,
                        crc_errors, exception_responses; double avg_response_time_ms; };
    Statistics get_statistics() const;
    void reset_statistics();
    std::string get_last_error() const;
};
```

---

## 10. Константы MIDAS (`constants.h`)

```
EQUIPMENT_NAME_SLOW_CONTROL = "SlowControl"
EQUIPMENT_NAME_SYNC         = "RunSync"

ODB_PATH_SETTINGS  = "/Equipment/SlowControl/Settings"
ODB_PATH_VARIABLES = "/Equipment/SlowControl/Variables"
ODB_PATH_ALARMS    = "/Alarms/SlowControl"

EVENT_ID_SLOW_CONTROL = 1
EVENT_ID_BOARD_DATA   = 2
EVENT_ID_EEPROM_CHECK = 3

BANK_NAME_BOARD_DATA = "BDAT"
BANK_NAME_STATISTICS = "STAT"
BANK_NAME_ALARMS     = "ALRM"
```

Пороги алармов (в единицах АЦП, требуют уточнения под реальное железо):
`SIPM_CURRENT_WARNING=3000 / ERROR=4000`, `SIPM_TEMP_WARNING=3500 / ERROR=4000`, `CPU_TEMP_WARNING=3000 / ERROR=3500`.

---

## 11. Стратегия опроса

**Каждые 5 минут, для каждой платы:**
1. Read Input Registers 0–27 (счётчик обновлений, статус HV и trip, токи, температуры, CPU)
2. Read Holding Registers 0–100 (DAC, HV enable, усиление/trip, CSR, задержка, адрес)
3. Проверка свежести: счётчик в input 0 должен измениться с прошлого опроса

**Раз в сутки (фоновая задача):**
1. Read Input Registers 100–149 (ID термометров + CPU ID) и 150–245 (обратное чтение DAC)
2. Read Input Registers 250–761 (scratchpad), проверка суммы 0xAA55
3. Сверка с эталоном из `eeprom_reference.json` и с уставками
4. При несовпадении — аларм и/или восстановление (запись уставок → 103 → 105 → статус в holding 98)

**Обработка ошибок:**
- Таймаут → до 3 повторов → пометить плату offline + аларм, продолжить с остальными
- CRC-ошибка → немедленный повтор; при >10 подряд — подозрение на кабель/наводки, аларм
- Ошибка порта → закрыть/открыть порт; при неуспехе — отключить ветку, критический аларм

---

## 12. Что делать дальше (приоритеты)

1. **`ConfigLoader`** — реальный парсинг `board_map.json` и `eeprom_reference.json` через nlohmann/json
2. **`BoardRegistry`** — хранение 240 плат, поиск, трекинг состояния
3. **`RS485BranchManager`** — потоки по веткам, retry, статистика
4. **`PeriodicPoller`** — 5-минутный цикл, параллельный опрос
5. **MIDAS frontend** — `SlowControlEquipment`, банки, ODB
6. **EEPROM-валидация** — суточный цикл
7. **State machine** + алармы

Оценка до production-ready: ~2–3 месяца.

---

## 13. Тестовый стенд ДАНСС-2 (практическая информация от Игоря, сентябрь 2026)

### Modbus на реальном железе

- Плата электроники подключена к крейту VME через **`/dev/ttyACM3`**
- Сейчас отвечают адреса **1 и 2** (процессорные платы установлены; аналоговые платы привезены, но будут подключены позже, в сентябре)
- Скорость связи жёстко 19200 бод в текущей прошивке; в будущем прошивка получит регистр выбора скорости
- Карта регистров (см. §2) подтверждена прошивкой 1-в-1

### Доступ к оборудованию и серверам

| Ресурс | Адрес/данные |
|---|---|
| Лабораторный сервер (снаружи ФИАНа) | `amefodev@dansslab.ddns.net` |
| Лабораторный сервер (изнутри ФИАНа, включая wifi 2/3 этажа) | `server.danss.local` |
| VNC-дисплей на сервере | `:6` (пример: `ssh -f -L 5906:localhost:5906 amefodev@dansslab.ddns.net`, затем `vncviewer -geometry 2400x1600 :6`) |
| Управление розетками (изнутри сети) | `http://power0.danss.local`, пользователь `administrator` |
| Розетки | `[04] DANSS II Test Stand Power`, `[05] Test Stand 15V DC`, `[08] CVME1` |
| VME крейт-контроллер | `cvme1.danss.local` — i7-4700EQ (4 ядра) @2.4ГГц, 16 ГБ RAM, мост Tundra CA91C042; Debian buster/sid, кастомное ядро Linux 4.15.1; логин с тем же именем и паролем, что и на сервере |
| Загрузка крейта | по PXE → tftp с сервера, файлы в `/srv/tftp`, конфиг `pxelinux.cfg/cvme1.cfg` |
| Блок питания | OWON ODP6033, Ethernet `d2pwr1.danss.local` / `192.168.10.81`, управляющий порт **3000**, протокол SCPI (проверено `telnet d2pwr1 3000` → `*IDN?`) |

### Блок питания OWON ODP6033 — распределение каналов

- **ch1** — сейчас задействован для стенда (нерегулируемый блок заменён на этот)
- **ch3** — планируется под фиксированное питание плат электроники, 5.5–6 В
- Оставшиеся два канала — под высокое напряжение (HV) для SiPM

### Драйвер и ПО VME/WFD

- Поддержка моста Tundra CA91C042 есть в апстриме, но рекомендуется форк лаборатории (Дима Калинкин правил драйвер, но апстрим не принял последние изменения): `github.com/lab305itep/linux`, ветка `vme_patches2_rebase2`, каталог `drivers/vme/bridges`
- Модули WFD (waveform digitizer) описаны в двух документах на Overleaf (ссылки хранятся в переписке, не дублируются здесь по сроку жизни)
- Штатного ПО для работы с WFD за 12 лет не появилось — используется отладочный код: `github.com/lab305itep/uwfd64`

### Что это означает для проекта

- Дефолтный/тестовый `board_map.json` для работы на стенде должен указывать `device: "/dev/ttyACM3"` и содержать платы с адресами 1 и 2 (а не полную конфигурацию на 240 плат) — конфиг на 240 плат остаётся целевым для финальной установки на детекторе
- Общая архитектура (`RS485BranchManager` на несколько веток `/dev/ttyUSB*`) не меняется — на стенде это частный случай с одной активной веткой на `ttyACM3`
- Интеграция с блоком питания OWON ODP6033 по Ethernet/SCPI — потенциальная дополнительная задача (не входит в Modbus/RS-485 часть, отдельный протокол и канал)

---

## 14. Окружение пользователя

- Рабочая директория проекта: `~/work/danss/danss-slow-control`
- MIDAS: `~/work/danss/packages/midas`
- Компилятор: `/usr/bin/c++` (GCC 11+), Boost 1.83.0
- Сборка: `~/work/danss/danss-slow-control/build`
- Известное безвредное предупреждение: `modbus_scanner.cpp:159` — сравнение `int` и `size_t` (можно поправить приведением `found_count` к `size_t`)

---

## 15. Управление блоком питания OWON ODP6033 (Slow Control)

Отдельная задача, добавлена в сентябре 2026 по итогам переписки с Игорем (§13):
нужен SC для блока OWON ODP6033, который подаёт ~6В на CPU-платы электроники и
~55В HV на SiPM.

### Железо и протокол

- **OWON ODP6033**: 3 канала — CH1, CH2 (0–60В / 3А каждый), CH3 (0–6В / 3А)
- Управление по Ethernet, **SCPI поверх сырого TCP-сокета**, порт **3000**
  (подтверждено `telnet d2pwr1 3000` → `*IDN?` в переписке)
- Адрес на стенде: `d2pwr1.danss.local` / `192.168.10.81:3000`
- Маппинг каналов на задачи — **конфигурируемый** (см. `config/power_supply.json`),
  жёстко в код не зашит. Пример для стенда: CH3 → CPU-платы (6В), CH1 → SiPM HV (55В),
  CH2 → резерв/не используется.
- Источник команд SCPI: `owon_odp_series_power_supply_programming_manual.pdf`
  (найден веб-поиском, ODP3033/ODP3063/ODP6033 одна прошивка команд).

### Архитектурные решения

1. **Отдельный MIDAS Equipment `PowerSupply`**, параллельно `SlowControl`/`RunSync`
   (не часть общего Modbus-опроса) — свой цикл опроса, чтобы TCP round-trip к
   блоку питания не тормозил Modbus-цикл по веткам RS-485.
2. **Два слоя**, по аналогии с `ModbusRTU`:
   - `OwonPSU` (`include/power/owon_psu.h`, `src/power/owon_psu.cpp`) — низкоуровневый
     SCPI/TCP-клиент: `connect()`/`disconnect()` с ограниченным по времени
     handshake, `select_channel()`, `set_voltage()`/`set_current()`,
     `set_voltage_protection()`/`set_current_protection()` (OVP/OCP),
     `set_output()`, измерения (`measure_voltage/current/power`,
     `measure_all_*`, `get_all_output_states`), статистика и `get_last_error()`.
     Таймаут на чтение ответа реализован тем же приёмом, что и в `ModbusRTU`:
     `select()` на файловом дескрипторе сокета, чтобы недоступный/выключенный
     блок питания не подвесил поток опроса.
   - `PowerSupplyController` (`include/power/power_controller.h`,
     `src/power/power_controller.cpp`) — политика поверх клиента:
     - `apply_protection_limits()` — выставляет OVP/OCP и ток-лимит на все
       каналы один раз при старте (не полагаемся на то, что осталось в
       памяти прибора с прошлой сессии);
     - `set_interlock_ready(bool)` / `is_interlock_ready()` — внешний интерлок;
       пока источник интерлока не определён (зависит от ещё не написанного
       `BoardRegistry` — идея: HV включается только когда платы SuperFGD online);
     - `enable_channel()` — для `SIPM_HV`: сначала обнуляет напряжение, включает
       выход, затем плавно поднимает до целевого шагами `ramp_step_v` каждые
       `ramp_interval_ms` (блокирующий вызов); для `CPU_POWER` — выставляет
       напряжение и включает выход сразу, без разгона;
     - `disable_channel()` — для HV плавно опускает до 0В перед выключением
       выхода; для CPU-питания выключает сразу;
     - `emergency_shutdown()` — мгновенно выключает все выходы без разгона
       (для перехода `ANY → STOPPED` и обработки аварий);
     - `poll()` — измеренные напряжение/ток/мощность + состояние выхода по
       каждому настроенному каналу, для цикла опроса SlowControl/MIDAS.
3. **Типы конфигурации** в `include/common/types.h`: `PSUChannelRole`
   (`UNUSED`/`CPU_POWER`/`SIPM_HV`), `PSUChannelConfig` (канал, роль, целевое
   напряжение, ток-лимит, OVP/OCP, шаг и интервал разгона, флаг интерлока),
   `PSUConfig` (host/port/каналы/таймауты), `PSUChannelStatus` (снимок
   измерений для одного канала). Плюс поле `SystemConfig::power_supply`.
4. **Константы** в `include/common/constants.h`: порт по умолчанию 3000,
   таймауты подключения/ответа, число каналов, дефолтный шаг/интервал
   разгона (1В / 500мс), имя MIDAS Equipment `"PowerSupply"`, пути ODB,
   `EVENT_ID_POWER_SUPPLY`, `BANK_NAME_PSU_DATA = "PSUD"`, допуски по
   напряжению/марже OVP/OCP.
5. **Пример конфига** `config/power_supply.json` — host/port/таймауты + 3 канала
   (CH3=CPU 6В, CH1=SiPM HV 55В с `require_interlock: true`, CH2=не используется).
6. **`ConfigLoader::load_power_config()`** — объявлен в `config_loader.h`,
   реализация пока заглушка (возвращает дефолты), как и остальной `ConfigLoader`.
7. **`src/power/power_equipment.cpp`** — заглушка с описанием плана интеграции
   в MIDAS (Фаза 4): `init()`/`read_event()`/интерлок из `SlowControl`/хуки
   на переходы `RunStateMachine`.
8. **CMakeLists.txt** обновлён: `include/power` в include-путях, новый
   `POWER_SOURCES` (`owon_psu.cpp`, `power_controller.cpp`) добавлен в
   `danss_core`, `power_equipment.cpp` — во `FRONTEND_SOURCES`.

### Статус проверки

Новые `.cpp`/`.h` пропущены через `g++ -std=c++17 -Wall -Wextra -fsyntax-only`
(с реальными заголовками boost::asio) — компилируются чисто, без предупреждений.
**Полная сборка через CMake+MIDAS и проверка на реальном блоке OWON ODP6033
(на стенде, `d2pwr1.danss.local:3000`) ещё не выполнялись** — это следующий шаг.

### Открытые вопросы (не решены, отслеживаются в TODO.md, раздел "Phase 2b")

- Источник и логика интерлока (`set_interlock_ready`) — привязка к состоянию
  плат/детектора ещё не спроектирована, ждёт `BoardRegistry`
- Реальные значения шага/интервала разгона HV (сейчас placeholder 1В/500-1000мс)
  — нужно уточнить у группы допустимую скорость нарастания HV на SiPM
- Реальный парсинг `power_supply.json` в `ConfigLoader`
- Верификация SCPI-команд на настоящем приборе (манускрипт найден в вебе,
  не локальная копия — стоит сверить с паспортом/шильдиком конкретного блока)
