# DANSS Slow Control & DAQ System

MIDAS-based slow control and data acquisition system for DANSS detector.

## Overview

- **Multi-branch RS-485**: Support for 4-8 RS-485 branches
- **Board Management**: ~240 detector boards with Modbus RTU protocol
- **Periodic Polling**: 5-minute cycles for currents, temperatures, trip status
- **EEPROM Validation**: Daily verification and automatic correction
- **Run Synchronization**: Strict Calibration ↔ Physics Run mode transitions

## Features

- Modbus RTU over RS-485 (Boost.Asio based)
- MIDAS 2.1 frontend integration
- JSON-based board configuration
- EEPROM reference data management
- Multi-threaded polling (one thread per RS-485 branch)
- MIDAS alarm system with future Telegram support

## Board Protocol (Modbus RTU)

### Supported Functions
- `0x03` - Read Holding Registers (DAC settings, CSR, counters)
- `0x04` - Read Input Registers (ADC, currents, temperatures)
- `0x06` - Write Single Register
- `0x10` - Write Multiple Registers

### Register Map (v2, October 2026)

Full map: [doc/MODBUS_MAP_v2.md](doc/MODBUS_MAP_v2.md).

Input (read only):
- `0` - update counter (increments at the end of every board cycle)
- `1` - HV status: trip per analog board (3 bits) + HV enable per channel (12 bits)
- `2` - gain and trip enable per analog board
- `3-14` - SiPM leakage currents (12 channels)
- `15-26` - SiPM temperatures (12 channels)
- `27` - CPU temperature
- `100-147` - thermometer IDs (12 × 4 registers), `148-149` - CPU ID
- `150-245` - DAC readback, `246` - CPU temperature sensor offset
- `250-761` - scratchpad readback (512 words)

Holding (read/write):
- `0-95` - DAC settings
- `96` - Counter ON/OFF (SiPM HV enable), bit per channel
- `97` - gain and trip enable, bit per analog board
- `98` - Global CSR (commands 101/103/105, status 0/10/20)
- `99` - delay between cycles
- `100` - device address (settable only from factory address 247)

## Requirements

### System
- Linux (tested on Ubuntu 20.04+, CentOS 7+)
- MIDAS 2.1 or later
- Boost 1.65+ (system, thread components)
- CMake 3.12+
- GCC 7+ or Clang 6+

### Hardware
- 4-8 × USB-RS485 adapters (/dev/ttyUSB0-7)
- RS-485 network with ~240 detector boards

## Building

```bash
# Set MIDAS environment
export MIDASSYS=/path/to/midas
export MIDAS_EXPTAB=/path/to/exptab

# Clone and build
git clone <repository-url> danss-slow-control
cd danss-slow-control
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Configuration

### Board Map (`config/board_map.json`)
Defines the mapping of boards to RS-485 branches and Modbus addresses:

```json
{
  "branches": [
    {
      "id": 0,
      "device": "/dev/ttyUSB0",
      "baud_rate": 19200,
      "boards": [
        {"address": 1, "name": "PMT-01-01"},
        {"address": 2, "name": "PMT-01-02"}
      ]
    }
  ]
}
```

### EEPROM Reference (`config/eeprom_reference.json`)
Contains reference thermometer IDs and CPU IDs for validation:

```json
{
  "boards": [
    {
      "name": "PMT-01-01",
      "thermometer_ids": ["28FF...", "28FF..."],
      "cpu_id": "ABC123"
    }
  ]
}
```

## Running

```bash
# Scan RS-485 bus to discover boards
./bin/modbus_scanner /dev/ttyUSB0

# Test communication with specific board
./bin/danss_frontend --test-modbus /dev/ttyUSB0 19200

# Start the frontend
./bin/danss_frontend -e <experiment_name>

# Monitor via MIDAS web interface
# http://localhost:8080
```

## Architecture

```
┌─────────────────┐
│  MIDAS Frontend │
│   (Main Loop)   │
└────────┬────────┘
         │
    ┌────┴─────┐
    │ RS-485   │
    │ Manager  │ (4-8 threads)
    └────┬─────┘
         │
    ┌────┴──────────┬──────────┬─────────┐
    │               │          │         │
┌───▼────┐    ┌────▼───┐  ┌───▼────┐  ┌▼─────┐
│Branch 0│    │Branch 1│  │Branch 2│  │ ...  │
│/ttyUSB0│    │/ttyUSB1│  │/ttyUSB2│  │      │
└───┬────┘    └────┬───┘  └───┬────┘  └──────┘
    │              │          │
   ~60 boards    ~60 boards ~60 boards
```

## Development Roadmap

### Phase 1: Core Infrastructure ✓
- [x] Project structure
- [x] CMake configuration
- [x] Modbus RTU implementation
- [ ] JSON configuration loader
- [ ] Board registry

### Phase 2: RS-485 Management
- [ ] Multi-branch manager
- [ ] Periodic poller (5-min cycle)
- [ ] EEPROM validator (daily check)

### Phase 3: MIDAS Integration
- [ ] Frontend skeleton
- [ ] SlowControl equipment
- [ ] MIDAS alarm integration
- [ ] State machine (Calibration ↔ Physics)

### Phase 4: Advanced Features
- [ ] Error recovery
- [ ] Logging and diagnostics
- [ ] Telegram notifications
- [ ] Unit tests

## License

[Specify license here]

## Authors

Aleksandr - Initial development

## Acknowledgments

- MIDAS DAQ framework
- Boost.Asio library
