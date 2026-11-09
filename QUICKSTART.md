# Быстрый старт

Обновлено 07.11.2026. Подробности — [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md), сборка — [doc/BUILD.md](doc/BUILD.md).

## 1. Собрать

```bash
git clone https://github.com/MrMefodij/danss_daq.git && cd danss_daq
git checkout amefodev-tests
export MIDASSYS=/path/to/midas            # коммит c69c0853930b…
mkdir -p build && cd build && cmake -DBUILD_TESTS=ON .. && make -j4 && ctest
```

Без MIDAS (утилиты, `danss_frontend --standalone`, тесты): `cmake -DWITH_MIDAS=OFF ..` — без
`MIDASSYS` выбирается само; см. «Без MIDAS» в [doc/BUILD.md](doc/BUILD.md).

## 2. Найти платы

```bash
ls /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbserial-* 2>/dev/null
./bin/modbus_scanner -q -s 1 -e 247 /dev/ttyACM3        # 38400 по умолчанию
```

Сканирование всех адресов — около 2.5 минут; если адреса примерно известны, сузьте диапазон
(`-s 1 -e 10`). Нет ответа нигде — проверьте скорость (38400), питание плат, кабель, A/B.

## 3. Посмотреть одну плату

```bash
./bin/modbus_tool /dev/ttyACM3 1 info                   # вся карта v2
./bin/modbus_tool /dev/ttyACM3 1 watch 10               # цикл платы (≈ 134 мс)
./bin/modbus_tool /dev/ttyACM3 1 sp-dump backup_1.json  # резервная копия ДО любых записей
```

Без аналоговых плат и счётчиков: температуры `n/a` (0x8001), ID термометров нули, обратное
чтение DAC = 0 — так и должно быть. Пока работает `danss_sc_fe` на этом порту, `modbus_tool` не
запускать. Остальные команды и план проверки — [doc/MODBUS_TOOL.md](doc/MODBUS_TOOL.md).

## 4. Блок питания (стенд, OWON 192.168.10.81)

```bash
./bin/psu_bench --config ../config/power_supply.json info     # только чтение
./bin/psu_bench --config ../config/power_supply.json limits   # записать пороги из конфига
```

Режимы `cpu-on`, `hv <V>`, `interlock <V>` включают выходы — только когда стенд к этому
готов. `test_owon_psu` на реальном приборе не запускать: поднимает 55 В и выключает всё.

## 5. MIDAS

```bash
export MIDAS_EXPTAB=$HOME/daq/online/exptab
./bin/danss_psu_fe -- --config ../config/power_supply.json
```

В mhttpd: `/Equipment/PowerSupply/Settings` — `Enable[2]` (ch3), `Interlock ready`,
`Enable[0]` (HV ch1); состояние — в `Readback`. Подробно — `PROJECT_CONTEXT.md` §10.

Платы:

```bash
./bin/danss_sc_fe -- --config ../config/board_map_stand.json
```

В mhttpd: `/Equipment/SlowControl/Readback` — сводка (`All healthy`), `Boards/<имя>` — по плате,
`Settings/Poll interval sec` — период опроса (меняется на лету), `Settings/Poll now`.
Ручные исключения — `Settings/Boards/<имя>/Exclude analog board`, `Exclude counter`,
`Exclusion reason`; сохраняются в `config/board_exclusions_*.json`. Для платы 17 на столе всё
уже исключено (`config/board_exclusions_bench.json`).

Уставки платы — там же: `DAC`, `HV enable`, `Gain trip enable`, `Cycle delay`. Изменение сразу
пишется на плату (не во время рана). Команды `Apply`, `Read from board`, `Save to flash`,
`Load from flash` — поставить `y`. Уставки сохраняются в `config/board_setpoints_*.json`;
при первом запуске берутся с платы.

## 6. Проверить конфиг плат

```bash
./bin/danss_frontend --standalone -c ../config/board_map_stand.json
```

Ошибка в конфиге печатается с путём до поля.
