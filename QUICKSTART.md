# Quick Start Guide

## Overview

This quick start guide will help you get the DANSS Slow Control system up and running for initial testing.

## Prerequisites

Before you begin, ensure you have:
- Linux system (Ubuntu 20.04+ or CentOS 7+)
- MIDAS 2.1 installed and configured
- Boost libraries (1.65+)
- CMake (3.12+)
- GCC 13+ or Clang 6+
- USB-RS485 adapters connected

## Step 1: Extract and Build

```bash
# Extract the project
tar xzf danss-slow-control.tar.gz
cd danss-slow-control

# Set MIDAS environment (if not already set)
export MIDASSYS=/opt/midas
export MIDAS_EXPTAB=/etc/exptab

# Create build directory
mkdir build
cd build

# Configure and build
cmake ..
make -j$(nproc)

# Verify build
ls -l bin/danss_frontend
```

## Step 2: Configure Serial Ports

```bash
# Check available USB-RS485 adapters
ls -l /dev/ttyUSB*

# Add your user to dialout group (one-time setup)
sudo usermod -a -G dialout $USER

# Log out and log back in for group changes to take effect
```

## Step 3: Scan RS-485 Bus

Before configuring, discover which boards are present on each branch:

```bash
# From build directory - scan first branch
./bin/modbus_scanner /dev/ttyUSB0
```

Expected output:
```
╔════════════════════════════════════════════════════════════╗
║         DANSS Modbus Address Scanner                      ║
╚════════════════════════════════════════════════════════════╝

[==================================================] 100% (30/30)

Active boards found: 5

Suggested board_map.json configuration:
{
  "boards": [
    {"address": 1, "name": "PMT-XX-01", "enabled": true},
    ...
  ]
}
```

**Scan all branches:**
```bash
for i in {0..7}; do
  ./bin/modbus_scanner /dev/ttyUSB$i > branch${i}_scan.txt
done
```

See [doc/MODBUS_SCANNER.md](doc/MODBUS_SCANNER.md) for detailed documentation.

## Step 4: Test Modbus Communication

Before running the full system, test communication with a specific board:

```bash
# From build directory - test board at address 1 (default)
./bin/danss_frontend --test-modbus /dev/ttyUSB0 19200

# Test board at specific address (e.g., address 5)
./bin/danss_frontend --test-modbus /dev/ttyUSB0 19200 5
```

Expected output:
```
╔════════════════════════════════════════════════════════════╗
║   DANSS Slow Control & DAQ System                         ║
║   MIDAS 2.1 Frontend                                      ║
╚════════════════════════════════════════════════════════════╝

[2025-01-19 17:40:00.123] [INFO] Testing Modbus communication on /dev/ttyUSB0
[2025-01-19 17:40:00.234] [INFO] Serial port opened successfully

Trying to communicate with board at address 5...
Checking address: 5
[2025-01-19 17:40:00.345] [INFO] Successfully read 10 registers from board address 5:
  Register 0: 0
  Register 1: 0
  Register 2: 1234
  Register 3: 1235
  ...
```

If you see timeout errors:
- Verify board is powered on
- Check RS-485 wiring (A, B, GND)
- Ensure termination resistors are in place
- Try increasing timeout in board_map.json
- **Try scanning to find active addresses:**
  ```bash
  ./bin/modbus_scanner --show-addresses /dev/ttyUSB0
  ```
- **Test specific address found by scanner:**
  ```bash
  ./bin/danss_frontend --test-modbus /dev/ttyUSB0 19200 <address>
  ```

## Step 5: Configure Your System

### Edit Board Map

```bash
cd ../config

# For testing with a few boards, edit the example:
nano board_map.json

# Or generate configuration for all 240 boards:
python3 generate_full_config.py > board_map_full.json
python3 generate_full_config.py --eeprom > eeprom_reference_full.json
```

### Key configuration parameters:

```json
{
  "system": {
    "poll_interval_sec": 300,           # 5 minutes
    "eeprom_check_interval_sec": 86400, # 24 hours
    "modbus_timeout_ms": 1000,          # Adjust if needed
    "max_retries": 3
  }
}
```

## Step 6: Run Standalone Test

Test the system without MIDAS integration:

```bash
cd ../build

# Run in standalone mode with verbose logging
./bin/danss_frontend --standalone -v
```

This will:
- Load configuration
- Initialize RS-485 branches
- Start polling cycle (not yet implemented - coming in Phase 2)

## Step 7: Integration with MIDAS

Once standalone testing is successful:

```bash
# Start MIDAS frontend
./bin/danss_frontend -e DANSS
```

## Project Status

### ✅ Phase 1 Complete
- [x] Project structure
- [x] CMakeLists.txt
- [x] ModbusRTU implementation
- [x] Configuration examples
- [x] Basic logging

### 🚧 Phase 2 In Progress
- [ ] RS485 Manager (multi-branch)
- [ ] Board Registry
- [ ] Periodic Poller
- [ ] EEPROM Manager

### 📋 Phase 3 Planned
- [ ] MIDAS Integration
- [ ] State Machine
- [ ] Alarms

See [TODO.md](TODO.md) for complete roadmap.

## Next Steps

1. **Test Modbus communication** with your hardware
2. **Generate full configuration** for 240 boards
3. **Implement RS485Manager** (see TODO.md Phase 2)
4. **Implement PeriodicPoller** for 5-minute cycle
5. **Add EEPROM validation**

## File Structure

```
danss-slow-control/
├── CMakeLists.txt         # Build configuration
├── README.md              # Project overview
├── TODO.md                # Development roadmap
├── QUICKSTART.md          # This file
├── config/
│   ├── board_map.json     # Board configuration (example)
│   ├── eeprom_reference.json  # EEPROM validation data
│   └── generate_full_config.py # Config generator
├── doc/
│   └── BUILD.md           # Detailed build instructions
├── include/               # Header files
│   ├── common/            # Common types, constants, logger
│   ├── modbus/            # ModbusRTU interface
│   └── eeprom/            # EEPROM management
├── src/                   # Implementation files
│   ├── frontend/          # MIDAS frontend
│   ├── modbus/            # Modbus implementation ✅
│   ├── rs485/             # RS-485 manager (TODO)
│   ├── eeprom/            # EEPROM validator (TODO)
│   ├── slowcontrol/       # Polling logic (TODO)
│   ├── sync/              # State machine (TODO)
│   └── utils/             # Utilities ✅
└── tests/                 # Unit tests (TODO)
```

## Getting Help

### Common Issues

**Problem**: CMake can't find MIDAS
**Solution**: Set MIDASSYS environment variable

**Problem**: Serial port permission denied
**Solution**: Add user to dialout group and reboot

**Problem**: Modbus timeout
**Solution**: Check hardware connections and increase timeout

**Problem**: Build errors
**Solution**: Ensure GCC 7+ and Boost 1.65+ are installed

### Development

This is a work-in-progress project. Core Modbus functionality is complete and tested. RS-485 multi-branch management and periodic polling are next priorities.

See TODO.md for detailed development plan and contribute accordingly.

## Contact

For questions about the DANSS detector system, contact your team lead.

For software issues, check the git repository issues page.
