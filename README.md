# DANSS Slow Control & DAQ System

MIDAS-based slow control for the DANSS-2 test stand and the DANSS detector:
SiPM electronics boards over Modbus RTU / RS-485 and the OWON ODP6033 power supply
(SiPM bias HV and electronics power) over SCPI/TCP.

Full project context (status, decisions, hardware facts): [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md) (Russian).

## What works (November 2026)

| Component | Status |
|---|---|
| `ModbusRTU` — Modbus RTU master (length-based framing, CRC, exceptions, statistics) | tested on hardware |
| Register map v2 (`constants.h`, `types.h`), `BoardClient` (CSR, scratchpad, verified DAC writes, address assignment) | tested on board 17 |
| `modbus_scanner` — find boards on a bus | tested on hardware |
| `modbus_tool` — inspect and service one board | tested on board 17 |
| `ConfigLoader` — `board_map.json`, `eeprom_reference.json`, `power_supply.json` with validation | unit tests |
| `OwonPSU` + `PowerSupplyController` — thresholds, interlock, HV ramps, emergency off | tested on the OWON (no load) |
| `danss_psu_fe` — MIDAS (TMFE) frontend for the power supply | tested on MIDAS with the OWON emulator |
| Board polling (`BoardRegistry`, `PeriodicPoller`, `DataCollector`) | tested on board 17 and the multi-board emulator |
| `danss_sc_fe` — MIDAS (TMFE) frontend for the boards (equipment `SlowControl`): currents in µA, temperatures in °C, analog board / counter presence, alarms | tested on board 17 and on MIDAS with the board emulator |
| Manual exclusions of analog boards / counters (ODB ↔ `config/board_exclusions_*.json`) | tested on MIDAS with the board emulator |
| Board control from MIDAS: DAC, HV enable, gain/trip, cycle delay; Apply / Read from board / Save to / Load from flash; locked during a run (ODB ↔ `config/board_setpoints_*.json`) | tested on MIDAS with the board emulator |
| EEPROM/scratchpad daily check, run state machine, WFD frontend | planned |

## Board protocol

Modbus RTU, **38400 baud, 8N1**. Functions 0x03, 0x04, 0x06, 0x10.
Register map v2 (input 0–27 status, 100–149 IDs, 150–245 DAC readback, 250–761 scratchpad;
holding 0–95 DAC, 96 HV enable, 97 gain/trip enable, 98 CSR, 99 cycle delay, 100 address):
see [doc/MODBUS_MAP_v2.md](doc/MODBUS_MAP_v2.md).

## Requirements

- Linux (cvme1: Linux Mint 19.1 with a conda GCC 13 toolchain; Ubuntu 22.04/24.04) or macOS for the utilities
- MIDAS, commit `c69c0853930b226e96090725a0ba5cb2b4293a14` (same on all machines)
- Boost (Asio; system, thread), nlohmann/json (fetched by CMake), CMake 3.16+, C++17 compiler

## Building

```bash
export MIDASSYS=/path/to/midas
mkdir -p build && cd build
cmake -DBUILD_TESTS=ON ..
make -j4 && ctest
```

Targets: `danss_core`, `danss_frontend`, `modbus_scanner`, `modbus_tool`, `danss_psu_fe`, `danss_sc_fe`;
with tests: `test_config_loader`, `test_psu_worker`, `test_owon_psu`, `psu_bench`, `test_poller`.
Without MIDAS (utilities, `danss_frontend --standalone`, tests): `cmake -DWITH_MIDAS=OFF ..` —
chosen automatically when `MIDASSYS` is not set. cvme1, offline builds: [doc/BUILD.md](doc/BUILD.md).

## Running

```bash
./bin/modbus_scanner -q -s 1 -e 247 /dev/ttyACM3          # find boards
./bin/modbus_tool /dev/ttyACM3 1 info                     # one board, full register map
./bin/modbus_tool /dev/ttyACM3 1 sp-dump backup_1.json    # backup before any write
./bin/psu_bench --config ../config/power_supply.json info # power supply, read-only
./bin/danss_psu_fe -- --config ../config/power_supply.json
./bin/danss_frontend --standalone -c ../config/board_map_bench.json  # poll boards without MIDAS
./bin/danss_sc_fe -- --config ../config/board_map_stand.json
```

On macOS the serial port is `/dev/cu.usbserial-*`. Without hardware:
`tests/fake_modbus_slave.py` (board emulator on a pty) and `tests/mock_owon_server.py` (OWON emulator).

Docs: [MODBUS_TOOL.md](doc/MODBUS_TOOL.md), [MODBUS_SCANNER.md](doc/MODBUS_SCANNER.md),
[ARCHITECTURE.md](doc/ARCHITECTURE.md), [BUILD.md](doc/BUILD.md), [QUICKSTART.md](QUICKSTART.md), [TODO.md](TODO.md).

## Configuration

- `config/board_map.json` — RS-485 branches and boards (`config/board_map_stand.json` for the DANSS-2 stand)
- `config/eeprom_reference.json` — reference thermometer and CPU IDs
- `config/power_supply.json` — OWON address and per-channel role, target, current, OVP/OCP, ramp, interlock

`ConfigLoader` rejects invalid files with the file name and field path, e.g.
`power_supply.json: power_supply.channels[0].ocp_limit_a: 3.2 вне допустимого диапазона 0.001…3.1`.
Power-supply limits are checked against the ODP6033 (CH1/CH2 ≤ 60 V, CH3 ≤ 6 V, OCP ≤ 3.1 A),
OVP must exceed the target, and SIPM_HV channels require the interlock.

## Architecture

```
MIDAS server (ODB, mserver, mlogger, mhttpd)
   ▲                        ▲
danss_psu_fe               danss_sc_fe
 PsuWorker                  PeriodicPoller / BoardRegistry
 PowerSupplyController      BoardClient
 OwonPSU ── TCP ── OWON     ModbusRTU ── RS-485 ── boards
```

Separate programs for the power supply and the boards, so a failure in one does not affect the other.

## License

GPL-3.0, see [LICENSE](LICENSE).

## Authors

- Aleksandr Mefodev — architecture, development
- Mariya Kolupanova — Modbus and power-supply development, debugging

## Acknowledgments

- MIDAS DAQ framework
- Boost.Asio, nlohmann/json
