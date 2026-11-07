# Сборка

Обновлено 07.11.2026.

## Зависимости

| Что | Версия | Примечание |
|---|---|---|
| MIDAS | коммит `c69c0853930b226e96090725a0ba5cb2b4293a14` | один и тот же на всех машинах (frontend и сервер) |
| Компилятор | C++17 (MIDAS сам собран как C++20) | на cvme1 — conda GCC 13.4, системный GCC 7.5 не трогаем |
| Boost | Asio + system, thread | Ubuntu 24.04 — 1.83; cvme1 — `libboost-devel` из conda |
| nlohmann/json | 3.12 | CMake скачивает сам (FetchContent) |
| CMake | 3.16+ | на cvme1 — 3.28 через `pip --user` |

## Обычная сборка

```bash
export MIDASSYS=/path/to/midas
mkdir -p build && cd build
cmake -DBUILD_TESTS=ON ..
make -j4
ctest                      # test_config_loader
```

MIDAS подключается через `find_package(Midas)` из `$MIDASSYS` (цель `midas::midas` несёт
пути к mvodb/midasio/mxml и zlib, с которыми MIDAS собран).

| Цель | Что это |
|---|---|
| `danss_core` | статическая библиотека: Modbus, BoardClient, PSU, PsuWorker, ConfigLoader, Logger |
| `danss_frontend` | CLI: `--test-modbus`, `--standalone -c board_map.json` (проверка конфига) |
| `modbus_scanner` | поиск плат на шине |
| `modbus_tool` | одна плата: чтение, запись, scratchpad, адрес |
| `danss_psu_fe` | MIDAS frontend блока питания (TMFE) |
| `test_config_loader`, `test_psu_worker`, `test_owon_psu`, `psu_bench` | с `-DBUILD_TESTS=ON` |

## cvme1 (Linux Mint 19.1)

MIDAS на cvme1 собран тулчейном conda (см. `PROJECT_CONTEXT.md` §16). Проект — тем же:

```bash
CONDA_PKGS_DIRS=$HOME/.conda/pkgs ~/miniforge3/bin/conda install -y -p ~/miniforge3/envs/gcc13 \
    -c conda-forge libboost-devel nlohmann_json
G=$HOME/miniforge3/envs/gcc13
export MIDASSYS=$HOME/daq/packages/midas
cd ~/daq/danss_daq && mkdir -p build && cd build
cmake -DCMAKE_CXX_COMPILER=$G/bin/x86_64-conda-linux-gnu-g++ \
      -DCMAKE_CXX_FLAGS="-isystem $G/include" \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
      -DBoost_ROOT=$G -DBUILD_TESTS=ON ..
make -j4 && ctest
```

`CONDA_PKGS_DIRS` нужен, потому что общая лабораторная настройка conda указывает кэш пакетов
в каталог без прав на запись.

## Утилиты без MIDAS и CMake

Для `modbus_tool`, `modbus_scanner`, `psu_bench` MIDAS не нужен.

```bash
# macOS (Homebrew: boost, nlohmann-json)
B=$(brew --prefix)
c++ -std=c++17 -O2 -Iinclude -I$B/include src/utils/modbus_tool.cpp src/modbus/modbus_rtu.cpp \
    src/modbus/modbus_crc.cpp src/modbus/board_client.cpp src/utils/logger.cpp -o modbus_tool
c++ -std=c++17 -O2 -Iinclude -I$B/include src/utils/modbus_scanner.cpp src/modbus/modbus_rtu.cpp \
    src/modbus/modbus_crc.cpp src/utils/logger.cpp -o modbus_scanner

# cvme1 (conda)
G=$HOME/miniforge3/envs/gcc13
$G/bin/x86_64-conda-linux-gnu-g++ -std=c++17 -O2 -Iinclude -isystem $G/include \
    tests/psu_bench.cpp src/power/owon_psu.cpp src/power/power_controller.cpp \
    src/utils/logger.cpp src/utils/config_loader.cpp -o psu_bench -static-libstdc++ -static-libgcc -pthread
```

## Проверка без железа

```bash
python3 tests/fake_modbus_slave.py --address 1 --state /tmp/board.json   # печатает /dev/pts/N
./bin/modbus_tool /dev/pts/N 1 info

python3 tests/mock_owon_server.py 15000 &
./bin/test_owon_psu 127.0.0.1 15000
./bin/test_psu_worker 127.0.0.1 15000
```

## Частые проблемы

- **`MIDASSYS environment variable not set`** — задайте `MIDASSYS` перед `cmake`.
- **`multiple definition of main` при сборке `danss_frontend`** — старый CMakeLists линковал
  `mfe`; в текущем этого нет.
- **`zlib.h not found` на cvme1** — conda-компилятор не видит `/usr/include`; используйте zlib
  из conda и `-isystem $G/include`.
- **Порт на macOS** — `/dev/cu.usbserial-*`, не `/dev/tty.*`.
- **Нет доступа к порту на Linux** — пользователь в группе `dialout` или `sudo`.
