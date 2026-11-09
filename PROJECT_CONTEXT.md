# DANSS Slow Control & DAQ — контекст проекта

> Этот файл предназначен для переноса контекста в новый чат.
> Вставьте его целиком (или приложите файлом) в начало нового диалога.
> Обновлён 07.11.2026.

---

## 0. Как использовать этот документ

Начните новый чат фразой вроде:

> «Продолжаем работу над проектом DANSS Slow Control. Ниже — полный контекст проекта.
> Текущая задача: <опишите задачу>.»

и приложите этот файл. Код — в репозитории **https://github.com/MrMefodij/danss_daq.git**,
рабочая ветка **`mkolupanova-tests`** (Мария Колупанова + наши правки). Карта регистров
плат — [doc/MODBUS_MAP_v2.md](doc/MODBUS_MAP_v2.md); утилита для одной платы —
[doc/MODBUS_TOOL.md](doc/MODBUS_TOOL.md).

---

## 1. О проекте

**Название:** DANSS Slow Control & DAQ Server
**Назначение:** медленный контроль (платы электроники SiPM, блок питания) и сбор данных
для тестового стенда ДАНСС-2 и детектора DANSS на базе MIDAS.
**Язык:** C++17 (код SC); MIDAS собран как C++20.
**Платформы:** Linux (cvme1 — Linux Mint 19.1, тулчейн conda GCC 13; лабораторный сервер —
Fedora 33); разработка и проверка с одной платой — macOS.

### Требования

| Параметр | Значение |
|---|---|
| Веток RS-485 | 4–8 в целевой конфигурации; **на стенде ДАНСС-2 — одна, `/dev/ttyACM3` на cvme1** |
| Плат всего | ~240 (~30 на ветку) в целевой конфигурации; на стенде — процессорные платы 1, 2 (3 запрограммирована) |
| Период опроса | 5 минут (платы), 10 с (блок питания) |
| Что опрашивается | токи утечки SiPM, температуры, статус HV и trip, уставки DAC и их обратное чтение |
| Энергонезависимая память | scratchpad платы (512 слов, сумма 0xAA55); суточная сверка с эталоном |
| Синхронизация | DAQ/SlowControl при переключении Calibration ↔ Physics Run |

### Технологический стек

- **MIDAS, коммит `c69c0853930b226e96090725a0ba5cb2b4293a14`** — один и тот же на всех машинах.
  Frontend'ы — на современном C++ API **TMFE** (`tmfe.h`), не на старом `mfe.cxx`.
- **Boost.Asio** (serial port, TCP) — 1.83 (Ubuntu 24.04 / conda на cvme1)
- **nlohmann/json** — FetchContent v3.12.0 (или из conda/Homebrew для ручной сборки)
- **CMake 3.16+**

---

## 2. Протокол плат — Modbus RTU over RS-485

**Параметры порта: 38400 бод, 8N1, без flow control — на всех платах, включая стенд**
(подтверждено 07.11.2026; в сентябре прошивка была на 19200). `DEFAULT_BAUD_RATE = 38400`.

Функции: `0x03` Read Holding, `0x04` Read Input, `0x06` Write Single, `0x10` Write Multiple.

### Карта регистров — версия 2 (октябрь 2026)

Полная карта, scratchpad, результаты проверки и открытые вопросы:
[doc/MODBUS_MAP_v2.md](doc/MODBUS_MAP_v2.md). На одну процессорную плату — 3 аналоговые
платы, 12 каналов SiPM.

**Input-регистры:** 0 счётчик обновлений · 1 статус HV `0ttt eeee eeee eeee` (trip по
аналоговой плате, HV по каналу) · 2 `0000 0rrr 0000 0aaa` (разрешение trip, усиление) ·
3–14 токи · 15–26 температуры · 27 температура CPU · 28–99 резерв · 100–147 ID термометров
(12 × 4) · 148–149 CPU ID · 150–245 обратное чтение DAC · 246 смещение датчика CPU ·
250–761 обратное чтение scratchpad.

**Holding-регистры:** 0–95 DAC · 96 SiPM HV enable (= «Counter ON/OFF») · 97 усиление и
разрешение trip · 98 Global CSR (101 загрузить / 103 стереть / 105 сохранить scratchpad;
статус 0 / 10 / 20) · 99 задержка между циклами · 100 адрес (только с заводского 247,
действует сразу после сохранения).

### Что известно о прошивке (проверено и подтверждено)

| Факт | Источник |
|---|---|
| 38400 8N1 на всех платах | Александр, 07.11.2026 |
| Цикл платы ≈ 134 мс при задержке 0; счётчик 16 бит (переполнение ≈ раз в 2.4 ч) | плата 17 |
| Ответ на чтение 28 регистров ≈ 25 мс | плата 17 |
| Стирание + сохранение scratchpad работают, сумма 0xAA55 сходится | плата 17 |
| Scratchpad загружается в рабочие регистры сам при включении питания | плата 17 |
| Holding 96 — HV enable; адрес действует сразу; резерв с 28 | Александр |
| Без аналоговых плат: обратное чтение DAC = 0, температуры 0x8001, ID термометров 0 | плата 17 |

Открыто (письмо Игорю 07.11.2026): признак «аналоговая плата подключена», единицы и
пересчёт токов/температур/CPU, раскладка каналов и битов по аналоговым платам, длительность
103/105, ограничения на частоту запросов.

### Тайминги

- Одна транзакция на 38400: ≈ 25 мс на 28 регистров (измерено), ≈ 40–60 мс на 101–125
- Плата (input 0–27 + holding 0–100) ≈ 70 мс; ветка из 30 плат ≈ 2 с; 8 веток параллельно ≈ 2 с
- Запас ≈ 150× относительно 5-минутного цикла

---

## 3. Архитектура

Решения — в [doc/ARCHITECTURE_DECISIONS.md](doc/ARCHITECTURE_DECISIONS.md) (Р1–Р12).

```
┌──────────────────────────────────────────────────────────┐
│  DAQ-сервер: MIDAS (ODB, mserver, mlogger, mhttpd)       │
└──────▲──────────────────────▲────────────────────────▲───┘
       │ TCP 1175 (mserver)   │                        │
┌──────┴──────────┐  ┌────────┴───────────┐  ┌─────────┴─────────┐
│ danss_psu_fe    │  │ SlowControl FE     │  │ WFD frontend      │
│ (готов)         │  │ danss_sc_fe        │  │ (cvme1, позже)    │
│ PsuWorker       │  │ PeriodicPoller     │  │                   │
│ PowerSupplyCtrl │  │ BoardRegistry      │  │                   │
│ OwonPSU (SCPI)  │  │ BoardClient        │  │                   │
└──────┬──────────┘  │ ModbusRTU          │  └───────────────────┘
       │ TCP 3000    └────────┬───────────┘
   OWON ODP6033               │ RS-485, 38400
   192.168.10.81          платы (адреса 1…246)
```

- **Отдельные программы** для блока питания и для плат (решение 11.2026): сбой опроса плат
  не мешает управлению HV и наоборот.
- **Слои Modbus:** `ModbusRTU` (кадры, CRC, таймауты) → `BoardClient` (карта v2, процедуры:
  CSR, scratchpad, DAC с проверкой, адрес) → `BoardRegistry`/`PeriodicPoller` (состояние
  плат, расписание, алармы) → MIDAS equipment.
- **Слои блока питания:** `OwonPSU` (SCPI/TCP) → `PowerSupplyController` (политика: пороги,
  разгон, интерлок) → `PsuWorker` (свой поток: долгие разгоны не блокируют MIDAS) →
  `danss_psu_fe` (TMFE, ODB, история, алармы).
- Модель многопоточности опроса плат: поток на ветку RS-485, общий снимок под мьютексом.

### State Machine (режимы)

```
STOPPED ⇄ PHYSICS_RUN ⇄ CALIBRATION_RUN
   └──────── MAINTENANCE
```

Автоматический цикл калибровки (калибровочный ран → подбор gain/HV → физика) — открытый
вопрос 6 в [doc/ARCHITECTURE_DECISIONS.md](doc/ARCHITECTURE_DECISIONS.md).

---

## 4. Структура проекта

```
danss_daq/
├── CMakeLists.txt                ✅ danss_core + danss_frontend, modbus_scanner, modbus_tool, danss_psu_fe, danss_sc_fe
├── PROJECT_CONTEXT.md            ← этот файл
├── README.md, QUICKSTART.md, TODO.md, PROJECT_SUMMARY.md (история), CONDA_GCC_INSTALLATION.md
├── config/
│   ├── board_map.json            ✅ пример (2 ветки)
│   ├── board_map_stand.json      ✅ стенд: /dev/ttyACM3, платы 1, 2, (3 выключена)
│   ├── board_map_bench.json      ✅ стол: плата 17 на Mac (/dev/cu.usbserial-1140)
│   ├── board_exclusions_*.json   ✅ ручные исключения (пишет danss_sc_fe)
│   ├── board_setpoints_*.json    ✅ уставки плат (пишет danss_sc_fe; создаётся при первом запуске)
│   ├── eeprom_reference.json     ✅ пример эталонов
│   ├── power_supply.json         ✅ OWON: 192.168.10.81, ch3 CPU 6 В, ch1 HV 55 В
│   └── generate_full_config.py   ✅ генератор на 240 плат
├── doc/
│   ├── ARCHITECTURE_DECISIONS.md ✅ журнал решений Р1–Р12
│   ├── STAND_TEST_2026-11-10.md  ✅ чек-лист проверки стенда с аналоговыми платами
│   ├── MODBUS_MAP_v2.md          ✅ карта регистров, проверка на плате 17, ответы Игоря 09.11
│   ├── MODBUS_TOOL.md            ✅ утилита одной платы, план проверки
│   ├── ARCHITECTURE.md, BUILD.md, MODBUS_SCANNER.md, SCANNER_*.md
├── include/
│   ├── common/   types.h, constants.h, logger.h, config_loader.h          ✅
│   ├── modbus/   modbus_rtu.h, board_client.h                              ✅
│   ├── rs485/    board_registry.h                                          ✅
│   ├── slowcontrol/ periodic_poller.h, data_collector.h, exclusions_file.h, setpoints.h ✅
│   └── power/    owon_psu.h, power_controller.h, psu_worker.h              ✅
├── src/
│   ├── modbus/   modbus_rtu.cpp, modbus_crc.cpp, board_client.cpp          ✅ проверены на плате 17
│   ├── utils/    logger.cpp, config_loader.cpp, modbus_scanner.cpp, modbus_tool.cpp ✅
│   ├── power/    owon_psu.cpp, power_controller.cpp, psu_worker.cpp        ✅ проверены на OWON
│   │             power_equipment.cpp                                       ⚠️ заглушка, можно удалить
│   ├── frontend/ psu_frontend.cxx (danss_psu_fe)                           ✅ проверен в облаке на MIDAS
│   │             sc_frontend.cxx (danss_sc_fe)                             ✅ проверен в облаке на MIDAS
│   │             main.cpp (--test-modbus, --standalone = опрос без MIDAS)  ✅
│   │             equipment.cpp                                             ⏳ заглушка
│   ├── rs485/    board_registry.cpp                                        ✅ плата 17 + имитатор
│   │             rs485_manager.cpp                                         ⚠️ заглушка (функция — в PeriodicPoller)
│   ├── slowcontrol/ periodic_poller.cpp (опрос + очередь команд), data_collector.cpp ✅ плата 17 + имитатор
│   │             exclusions_file.cpp, setpoints.cpp                        ✅ имитатор
│   ├── eeprom/   eeprom_manager.cpp, eeprom_validator.cpp                  ⏳ пусто
│   └── sync/     state_machine.cpp                                         ⏳ пусто
└── tests/
    ├── CMakeLists.txt            ✅ (cmake -DBUILD_TESTS=ON)
    ├── test_config_loader.cpp    ✅ 42 проверки, в ctest
    ├── test_psu_worker.cpp       ✅ 26 проверок против mock
    ├── test_owon_psu.cpp         ✅ 20 проверок против mock (на приборе НЕ запускать: 55 В)
    ├── psu_bench.cpp             ✅ пошаговая проверка OWON на стенде
    ├── test_poller.cpp           ✅ 64 проверки: единицы, DataCollector, файлы исключений и уставок, опрос и команды против имитатора; в ctest — без имитатора
    ├── mock_owon_server.py       ✅ эмулятор OWON (семантика выходов как у прибора)
    └── fake_modbus_slave.py      ✅ имитатор плат по карте v2 (pty; несколько адресов, --analog, --counters, --control: freeze/silent/reboot/trip/analog/counters)
```

---

## 5. Текущее состояние (09.11.2026)

### ✅ Готово и проверено на железе

- **ModbusRTU**: конец кадра по длине из кода функции, очистка входного буфера перед
  запросом, проверка адреса/кода функции/числа регистров/эха записи, ответ-исключение
  (5 байт), раздельная статистика; проверки числа регистров в рантайме (не `assert`).
- **Карта регистров v2** в `constants.h`/`types.h` (`BoardData`, `ScratchpadImage`).
- **BoardClient + modbus_tool** — на плате 17: вся карта, цикл, CSR, scratchpad, перезагрузка.
- **modbus_scanner** — карта v2, плата с ответом-исключением показывается как «answered
  with error», заводской адрес 247 помечается.
- **ConfigLoader** — разбор трёх JSON с проверками и сообщениями «файл: путь.до.поля: что не так».
- **Блок питания** (OwonPSU, PowerSupplyController) — на приборе без нагрузки: пороги с
  обратным чтением, отказ HV без интерлока и без питания электроники, разгон 0→55 В и
  спуск, снятие интерлока во время разгона.

### ✅ Готово, проверено без железа

- **danss_psu_fe + PsuWorker** — на MIDAS c69c085 в облаке с эмулятором OWON: ODB-настройки
  с проверкой, Enable/интерлок, отклонение недопустимых настроек, новая цель для включённого
  канала, JRPC `emergency_shutdown` во время разгона, перезапуск без переключения выходов.
  **На cvme1 и с реальным OWON ещё не запускался.**

- **Опрос плат** (`PeriodicPoller` → `BoardRegistry`, проверки `DataCollector`) — на плате 17
  (две перезагрузки: offline → online, перезагрузки посчитаны, порт переоткрыт) и на имитаторе
  нескольких плат (`test_poller`: stale, trip, reboot, silent, подключение аналоговой платы).
- **danss_sc_fe** — на MIDAS c69c085 в облаке с имитатором: ODB `Readback`/`Boards`, банки
  BDAT/SIPI/SIPT в истории (mlogger), алармы offline/stale/trip со сбросом, сообщение о
  перезагрузке, `All healthy`, смена периода опроса на лету, `Poll now`, остановка по Ctrl-C.
  На плате 17 (Mac, 07.11): алармы offline/сброс и перезагрузка при выключении питания.
  09.11: единицы (мкА, °C), признаки аналоговых плат и счётчиков, ручные исключения с файлом
  (в т.ч. восстановление после очистки ODB), политика alarm/warning; **управление платами** —
  правка уставок, отклонение недопустимых и во время рана, Save/Load flash, расхождение после
  перезагрузки → аларм → Apply, Read from board. **На cvme1 и со стендом ещё не запускался;
  запись уставок на плате 17 ещё не проверялась.**

### 🚧 Следующие шаги

1. `danss_psu_fe` и `danss_sc_fe` на cvme1 (OWON, стенд); сканирование стенда на 38400
   (2 ноября не ответила ни одна плата — сканировали на 19200).
2. Интерлок HV от `/Equipment/SlowControl/Readback/All healthy` (условия — решение группы).
3. Суточная сверка scratchpad/ID с эталоном; state machine ранов; WFD frontend.

---

## 6. Ключевые технические решения и подводные камни

- **Boost.Asio serial_port не имеет `available()`** — ожидание ответа через `select()` на
  `native_handle()`. Блокирующий `boost::asio::read` с `transfer_at_least` подвисал при
  отсутствии ответа.
- **Граница кадра Modbus RTU** определяется по длине (из кода функции и байта счёта), а не
  по паузе T3.5: USB-RS485 адаптеры доставляют байты пачками. Перед отправкой — `tcflush`.
- **CRC16**: полином `0xA001`, lookup-таблица, LSB первым.
- **OWON ODP6033 (FV V3.10.0):**
  - `OUTPut:STATe 0|1` включает/выключает **все каналы сразу**, `INST:NSEL` на неё не влияет;
    по одному каналу — только `CHANnel:OUTPut:ALL a,b,c` (и запрос `CHAN:OUTP:ALL?` → `1, 0, 1`).
    Ошибку нашёл реальный прибор: включение ch3 включало ch1 (HV) без интерлока.
  - `VOLT:LIM?` / `CURR:LIM?` поддерживаются (обратное чтение OVP/OCP); OCP максимум 3.1 А.
  - На cvme1 Boost.Asio не разрешает имя `d2pwr1.danss.local` — в конфигах IP.
- **MIDAS `Msg(MINFO, ...)`**: `MINFO`/`MERROR` — макросы с `__FILE__, __LINE__`; в тернарный
  оператор их не подставлять.
- **libmfe.a** приносит второй `main()` — `danss_frontend` линкуется только с `midas::midas`.
- **`danss::log`** переименован в `danss::logging`: имя `log` перекрывало `::log()` из `<cmath>`.
- В скриптах не использовать `pkill -f <имя>` — убивает и сам скрипт; `pkill -x <процесс>`.

---

## 7. Сборка и запуск

### Сборка (CMake)

```bash
export MIDASSYS=<путь к MIDAS c69c085>   # cvme1: ~/daq/packages/midas
mkdir -p build && cd build
cmake -DBUILD_TESTS=ON ..
make -j4 && ctest
```

Цели: `danss_core` (статическая), `danss_frontend`, `modbus_scanner`, `modbus_tool`,
`danss_psu_fe`, `danss_sc_fe`; с тестами — `test_config_loader`, `test_psu_worker`, `test_owon_psu`,
`psu_bench`, `test_poller`.
MIDAS подключается через `find_package(Midas)` (цель `midas::midas` несёт пути к
mvodb/midasio и zlib, с которыми MIDAS собран).

**cvme1** (системный gcc/g++ не трогаем — тулчейн conda, см. §16):

```bash
CONDA_PKGS_DIRS=$HOME/.conda/pkgs ~/miniforge3/bin/conda install -y -p ~/miniforge3/envs/gcc13 \
    -c conda-forge libboost-devel nlohmann_json
G=$HOME/miniforge3/envs/gcc13
cmake -DCMAKE_CXX_COMPILER=$G/bin/x86_64-conda-linux-gnu-g++ -DCMAKE_CXX_FLAGS="-isystem $G/include" \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" -DBoost_ROOT=$G -DBUILD_TESTS=ON ..
```

**Ручная сборка утилит без MIDAS** (Mac/cvme1):

```bash
c++ -std=c++17 -O2 -Iinclude -I<boost/json include> src/utils/modbus_tool.cpp src/modbus/modbus_rtu.cpp \
    src/modbus/modbus_crc.cpp src/modbus/board_client.cpp src/utils/logger.cpp -o modbus_tool
```

### Утилиты

```bash
modbus_scanner -q -s 1 -e 247 /dev/ttyACM3            # найти платы (38400 по умолчанию)
modbus_tool /dev/ttyACM3 1 info                       # одна плата: всё по карте v2
modbus_tool /dev/ttyACM3 1 sp-dump backup_1.json      # резервная копия до любых записей
psu_bench --config config/power_supply.json info      # блок питания: только чтение
danss_psu_fe -- --config config/power_supply.json     # MIDAS frontend блока питания
danss_sc_fe -- --config config/board_map_stand.json   # MIDAS frontend плат
danss_frontend --standalone -c config/board_map_bench.json  # опрос плат без MIDAS (таблица)
```

Порт на macOS — `/dev/cu.usbserial-*`. Без железа: `tests/fake_modbus_slave.py` (печатает
pty-устройство), `tests/mock_owon_server.py <port>`.

---

## 8. Конфигурация

Все три файла читает `ConfigLoader` (`include/common/config_loader.h`). Ошибка — исключение
`ConfigError` с файлом и путём до поля; неизвестное поле — предупреждение; отсутствующее
необязательное — значение по умолчанию из `constants.h`.

- **`board_map.json`**: `system` (интервалы, таймауты, повторы) и `branches[]` (`id`, `device`,
  `baud_rate` = 38400, `parity` N/E/O, `boards[]` с `address` 1–247, `name`, `enabled`).
  Проверки: адреса без повторов на ветке, имена уникальны, стандартные скорости, 247 —
  предупреждение.
- **`eeprom_reference.json`**: `boards[]` с `name`, `modbus_address`, `thermometer_ids`
  (16 hex-цифр), `cpu_id` (hex); приводятся к верхнему регистру.
- **`power_supply.json`**: `host` (IP), `port`, `poll_interval_sec` (10), `channels[]` с
  `role` CPU_POWER / SIPM_HV / UNUSED, целью, током, OVP, OCP, шагом и интервалом разгона,
  `require_interlock`. Проверки безопасности: ch1/ch2 ≤ 60 В и OVP ≤ 61 В, ch3 ≤ 6 В и
  OVP ≤ 6.6 В, OCP ≤ 3.1 А, OVP ≥ цель + 0.5 В, OCP ≥ ток, для SIPM_HV интерлок обязателен
  и шаг ≤ 5 В.

---

## 9. Ключевые структуры данных

- `BoardData` (`types.h`) — снимок платы по карте v2: `update_counter`, `hv_status`,
  `gain_trip_status`, 12 × (`leakage_current`, `temperature`, `thermometer_id`),
  `cpu_temperature`, `cpu_temp_offset`, `cpu_id`, `dac_readback[96]`, `dac_settings[96]`,
  `hv_enable`, `gain_trip_enable`, `global_csr`, `cycle_delay`, `device_address`,
  `data_valid`, `data_fresh`; помощники `hv_channel_enabled()`, `analog_board_tripped()`,
  `any_trip()`, `dac_mismatches()`.
- `ScratchpadImage` — 512 слов, `checksum_ok()`, доступ к полям.
- `BoardConfig`, `BranchConfig`, `SystemConfig`; `PSUConfig`, `PSUChannelConfig`,
  `PSUChannelStatus`; `RunMode`, `Alarm`, `BranchStats`.
- `ModbusRTU(device, baud_rate = 38400)`: `read_input_registers`, `read_holding_registers`,
  `write_single_register`, `write_multiple_registers`, статистика, `get_last_error()`.
- `BoardClient(bus, address)`: `read_status/settings/dac_readback/ids/scratchpad`,
  `write_dac/all_dac/hv_enable/...`, `wait_next_cycle`, `write_dac_verified`, `csr_command`,
  `save_to_scratchpad`, `load_from_scratchpad`, `erase_scratchpad`, `assign_address`.

---

## 10. MIDAS: equipment, ODB, банки

| Equipment | Программа | Event ID | Банк | Статус |
|---|---|---|---|---|
| `PowerSupply` | `danss_psu_fe` | 4 | `PSUD` (V, I, P, On × 3, float) | готов |
| `SlowControl` | `danss_sc_fe` | 1 | `BDAT` (9 × плата), `SIPI` мкА, `SIPT` °C (12 × плата), float | готов |

**ODB `/Equipment/PowerSupply`:**

- `Settings/` — `Host`, `Port`, `Poll interval sec`, таймауты; массивы по каналам (индекс =
  канал − 1): `Role`, `Name`, `Target V`, `Current limit A`, `OVP V`, `OCP A`, `Ramp step V`,
  `Ramp interval ms`, `Require interlock`. При первом запуске заполняются из JSON, дальше
  правятся в браузере; **каждое изменение проверяется ConfigLoader**, недопустимое
  отклоняется целиком (ошибка в `Readback/Settings error`, аларм).
- `Settings/Enable[3]` — команды (сбрасываются в `n`, если не выполнились);
  `Settings/Interlock ready` — ручной интерлок, при старте `n`.
- `Readback/` — `Connected`, `Protections ok`, `Interlock ready`, `Busy`, `Last error`,
  `Settings error`, `Output on[3]`, `Voltage V[3]`, `Current A[3]`.
- Алармы: «PowerSupply link», «PowerSupply chN voltage», «PowerSupply settings».
- JRPC `emergency_shutdown`. Остановка frontend выходы не трогает.

**ODB `/Equipment/SlowControl`** (`danss_sc_fe -- --config config/board_map.json`):

- Платы и ветки — только из JSON (ConfigLoader); порядок плат в банках = порядок в конфиге,
  платы с `enabled: false` в банках есть (status 4, остальное −1 / NaN).
- `Settings/` — `Config file`, `Exclusions file`; `Poll interval sec` (при первом запуске из
  JSON, дальше действует значение в ODB, меняется на лету); `Poll now` (y → опрос сразу);
  `Names BDAT|SIPI|SIPT` — подписи для истории.
- `Settings/Boards/<имя>/` — **ручные исключения**: `Exclude analog board[3]`,
  `Exclude counter[12]`, `Exclusion reason`. Каждое изменение — сообщение в журнал MIDAS
  (что, было, причина) и сразу запись в файл исключений (`system.exclusions_file` в
  board_map.json, путь от каталога конфига; атомарно). При старте **файл главнее ODB**; файла
  ещё нет — берутся значения из ODB и файл создаётся.
- `Readback/` — `Online`, `Stale`, `Offline`, `Disabled`, `With problems`, `With missing`,
  `Tripped`, `All healthy` (все включённые платы online и без проблем; исключённое не
  считается), `Missing policy`, `Last cycle`.
- `Boards/<имя>/` — `Status`, `Branch`, `Address`, `Update counter`, `Analog boards seen`,
  `Counters seen`, `Missing`, `Excluded`, `Problems`, `Last error`, `Reboots`, `Failures`,
  `Current uA[12]`, `Temperature C[12]`.
- `Variables/BDAT` — на плату 9 чисел: status (0 unknown, 1 online, 2 stale, 3 offline,
  4 disabled), update counter, CPU temp (raw), HV status (input 1), маска trip, маска видимых
  аналоговых плат, маска видимых счётчиков, число расхождений DAC, число перезагрузок; −1 —
  нет данных.
- `Variables/SIPI` — токи утечки, мкА (ADC × 6.6 / 1024); `SIPT` — температуры SiPM, °C
  (int16 / 16). NaN — нет данных (плата не отвечает / нет доступа к термометру); mhttpd
  такие точки не рисует.
- **Состав** (письмо Игоря 09.11.2026): аналоговая плата видна, если не все 32 обратных чтения
  её DAC = 0; счётчик — если ID термометра ≠ 0; счётчик есть, а температура 0x8001 — ошибка
  температуры. Всё, что не исключено вручную, должно быть на месте. Реакция на отсутствие —
  `system.missing_component`: `"alarm"` (по умолчанию) или `"warning"` (только сообщение);
  **политика ещё обсуждается** (Игорь: аларм, отключать только вручную).
- Алармы (класс `Alarm`): «SC <плата> offline», «stale», «trip», «DAC», «missing»,
  «SC exclusions file» (не удалось записать файл). Каждое срабатывание дублируется сообщением
  MERROR: MIDAS пишет TALK класса не чаще раза в 60 с. Перезагрузка платы — сообщение.
- **Уставки плат (s2-07, Р12)** — `Settings/Boards/<имя>/`: `DAC[96]`, `HV enable`,
  `Gain trip enable`, `Cycle delay` — желаемые значения; сохраняются в `system.setpoints_file`
  (при старте файл главнее ODB). Правка оператора проверяется (HV enable — биты 0–11,
  gain/trip — маска 0x0707, всё 0…65535) и сразу пишется на плату — только изменённые группы,
  с чтением holding обратно; в журнал — что было и что стало. Команды (y, сбрасываются сами):
  `Apply` (все уставки ODB → плата), `Read from board` (плата → ODB, на плату не пишет),
  `Save to flash` (103→105 с проверкой scratchpad; отказ, если плата отличается от ODB),
  `Load from flash` (101). Во время рана всё это запрещено (`system.setpoints_during_run`).
  Уставок платы нет в файле — при первом ответе берутся с платы. Плата разошлась с ODB сама
  (перезагрузка со старым flash, `modbus_tool`) — на плату ничего не пишется, аларм
  «SC <плата> setpoints»; решает оператор: `Apply` или `Read from board`. Состояние —
  `Boards/<имя>/Setpoints` («in sync» / «ODB -> board: …»), `Last command`.
  Запись выполняет поток опроса ветки (очередь команд, сразу, между опросами), после записи
  ждёт цикл платы и опрашивает её заново.
- «Данные не обновляются» — только если счётчик стоит ≥ 1 с + 3 × `Cycle delay` (два опроса
  подряд, например после команды, могут попасть в один цикл платы).
- Цикл: поток опроса (статус, уставки, обратное чтение DAC, ID термометров) по завершении
  цикла ставит флаг, periodic-обработчик (1 с) публикует событие и ODB. JRPC `poll_now`.

---

## 11. Стратегия опроса плат

**Каждый цикл** (`poll_interval_sec`, на стенде 300 с; меняется в ODB), для каждой платы:
input 0–27 (состояние), holding 0–100 (уставки), input 150–245 (обратное чтение DAC),
input 100–149 + 246 (ID термометров и CPU) — 5 транзакций; ограничений по частоте нет (Игорь).
Свежесть — счётчик input 0 изменился (16 бит, сравнивать на «изменился»); «stale» — если стоит
≥ 1 с + 3 × cycle delay. Сброс счётчика — перезагрузка: плата сама берёт уставки из flash,
frontend на неё не пишет, а сверяет с ODB (расхождение — аларм «setpoints»).

**Команды платам** (уставки, flash) — в очереди ветки, выполняются её потоком сразу, между
опросами; после команды — ожидание цикла платы и повторный опрос.

**Раз в сутки (план, s2-03):** scratchpad (input 250–761, сумма 0xAA55), сверка ID с
эталоном (`eeprom_reference.json`).

**Состав** (письмо Игоря 09.11.2026): аналоговая плата видна, если не все 32 обратных чтения её
DAC = 0; счётчик — если ID термометра ≠ 0; температура 0x8001 при ID ≠ 0 — ошибка температуры.
Всё, что не исключено оператором вручную, должно быть на месте; DAC и trip сверяются только у
видимых и не исключённых аналоговых плат.

**Ошибки:** таймаут → до 3 повторов → плата offline + аларм; CRC → повтор, >10 подряд —
аларм «кабель/наводки»; ошибка порта → переоткрыть, иначе отключить ветку + критический аларм.

---

## 12. Приоритеты

1. `danss_psu_fe` и `danss_sc_fe` на cvme1; сканирование стенда на 38400
2. Интерлок HV от `SlowControl/Readback/All healthy` (условия — решение группы)
3. Ответы Игоря: признак аналоговой платы, единицы, раскладка каналов
4. Суточная сверка; RunStateMachine; WFD frontend на cvme1

Трекер задач: артефакт «План работ DANSS DAQ» в Claude.

---

## 13. Тестовый стенд ДАНСС-2

| Ресурс | Адрес/данные |
|---|---|
| Лабораторный сервер (снаружи) | `amefodev@dansslab.ddns.net` |
| Лабораторный сервер (внутри) | `server.danss.local` — Fedora 33 (EOL); chronyd |
| VNC | `:6` (`ssh -f -L 5906:localhost:5906 amefodev@dansslab.ddns.net`) |
| Розетки | `http://power0.danss.local`: [04] Test Stand Power, [05] Test Stand 15V DC, [08] CVME1 |
| VME крейт-контроллер | `cvme1.danss.local` — i7-4700EQ, 16 ГБ, мост Tundra CA91C042; **Linux Mint 19.1** (bionic, glibc 2.27), GCC 7.5, ядро 4.15.1; загрузка по PXE, корень по NFS |
| RS-485 к платам | на cvme1 `/dev/ttyACM3` (адаптер WCH, порт if06), **38400** |
| Блок питания | OWON ODP6033, **`192.168.10.81:3000`** (`d2pwr1.danss.local`), SCPI, FV V3.10.0 |

- **OWON, каналы:** ch3 — питание электроники 6 В (максимум канала 6 В), ch1 — HV SiPM 55 В,
  ch2 — резерв. Рабочие пороги в приборе: ch1 OVP 57 В / OCP 0.7 А, ch3 OVP 6.5 В / OCP 3.1 А.
  На 07.11.2026 к OWON ничего не подключено.
- **Платы:** процессорные 1, 2 на стенде, 3 — запрограммирована; плата 17 — у Александра для
  проверок (к ней ничего не подключено). 2 ноября на стенде не ответила ни одна плата на
  19200 — повторить на 38400.
- **VME/WFD:** драйвер моста — форк `github.com/lab305itep/linux`, ветка
  `vme_patches2_rebase2`; отладочный код WFD — `github.com/lab305itep/uwfd64`.

---

## 14. Окружение

- **Mac (разработка):** репозиторий `~/work/danss/DAQ/danss_daq`; MIDAS (другой коммит)
  `~/work/danss/packages/midas`; адаптер `/dev/cu.usbserial-1140`.
- **cvme1:** репозиторий `~/daq/danss_daq`; MIDAS `~/daq/packages/midas` (c69c085);
  тестовый эксперимент `~/daq/online`; тулчейн `~/miniforge3/envs/gcc13`; CMake 3.28 в
  `~/.local/bin`.

---

## 15. Блок питания OWON ODP6033

**Слои:** `OwonPSU` (SCPI/TCP, таймауты через `select()`, переподключение после сбоя) →
`PowerSupplyController` (пороги с обратным чтением, интерлок, разгон/спуск HV, аварийное
выключение, сверка порогов после переподключения) → `PsuWorker` (поток, неблокирующие
команды, прерывание разгона) → `danss_psu_fe` (§10).

**Правила безопасности в коде:**
- HV (`SIPM_HV`) включается только при интерлоке, подтверждённых OVP/OCP и включённом
  питании электроники (ch3); начинается с 0 В (или с текущего напряжения, если выход включён).
- Снятие интерлока или «выключить» во время разгона — разгон прерывается, канал плавно
  опускается. Аварийное выключение — `OUTPut:STATe 0` (все выходы), с проверкой и повтором.
- Один канал включается/выключается через `CHANnel:OUTPut:ALL` с обратным чтением.

**Интерлок сейчас** — программный флаг, ручной (`Settings/Interlock ready`). Не аппаратная
защита (её дают OVP/OCP прибора) и не страховка от падения программы. Целевой источник —
SlowControl по состоянию плат (на связи, `Update#` растёт, нет trip, температуры в норме);
условия — решение группы до подключения SiPM.

**Проверено на приборе (02.11.2026, без нагрузки):** `psu_bench` — info, limits (с `--config`),
cpu-on/off, hv 5, interlock 6, hv 55 (разгон 27 с при 1 В / 0.5 с, выдержка 54.99 В).

**Открыто:** проверка на нагрузке (SiPM); допустимая скорость нарастания HV (в конфиге
1 В / 1000 мс → ≈ 55 с); во время разгона `Readback/Voltage V` не обновляется.

---

## 16. MIDAS на cvme1 и DAQ-сервере

- **Разделение:** на cvme1 только то, что рядом с железом (VME-драйвер, WFD frontend);
  ODB, mserver, mlogger, mhttpd — на DAQ-сервере; frontend подключается по TCP 1175.
  Event builder — на отдельном сервере (Р6).
- **Правило:** frontend и сервер MIDAS — из одного коммита (`c69c0853930b…`).
- **cvme1: системные gcc/g++ не менять.** Отвергнуто: g++-13 из PPA (тянет libstdc++6 от
  GCC 16, PPA удалён), кросс-сборка, старый MIDAS, системный zlib с conda-компилятором.
- **Выбрано:** CMake 3.28 через `pip --user`; Miniforge `~/miniforge3`, окружение `gcc13`
  (GCC 13.4, без sudo, `conda init` не запускали); `-static-libstdc++ -static-libgcc`; zlib
  из conda через `-isystem $Z/include` и RPATH (каталог `miniforge3` удалять нельзя).

```bash
G=$HOME/miniforge3/envs/gcc13/bin; Z=$HOME/miniforge3/envs/gcc13
cd ~/daq/packages/midas/build && rm -rf *
cmake -DCMAKE_C_COMPILER=$G/x86_64-conda-linux-gnu-gcc -DCMAKE_CXX_COMPILER=$G/x86_64-conda-linux-gnu-g++ \
      -DCMAKE_C_FLAGS="-isystem $Z/include" -DCMAKE_CXX_FLAGS="-isystem $Z/include" \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
      -DCMAKE_SHARED_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
      -DZLIB_INCLUDE_DIR=$Z/include -DZLIB_LIBRARY=$Z/lib/libz.so -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON ..
make -j4 && make install
```

- **Статус:** MIDAS собран и работает на cvme1 (mhttpd запускается). Часы cvme1 с сервером
  синхронизированы. На сервере (`server.danss.local`, Fedora 33) — окружение conda `daq` с
  ROOT 6.40.04 (C++20) для сборки MIDAS с ROOT; сборка MIDAS там не подтверждена.
- Сетевой доступ cvme1 → сервер на порт mserver проверить при запуске WFD frontend.

---

## 17. Журнал проверок на железе

| Дата | Что | Результат |
|---|---|---|
| 02.11.2026 | Стенд, `modbus_scanner` на ttyACM0–3, ttyUSB2–6, 19200 | ни одна плата не ответила (скорость — 38400) |
| 02.11.2026 | OWON, `psu_bench` | найдено: `OUTP:STAT` = все каналы, `OUTP:STAT:ALL?` одно число, OCP ≤ 3.1 А, имя `.local` не разрешается; исправлено; разгон до 55 В |
| 07.11.2026 | Плата 17 на Mac, 38400 | карта v2 без ошибок; цикл 134 мс; scratchpad сохранение и автозагрузка; DAC readback = 0 без аналоговых плат |
