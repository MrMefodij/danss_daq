# Development TODO

## Phase 1: Core Infrastructure ✓

- [x] Project structure
- [x] CMakeLists.txt configuration
- [x] ModbusRTU class with Boost.Asio
- [x] CRC16 implementation
- [x] Common types and constants
- [x] JSON configuration examples
- [x] Configuration generator script
- [x] Modbus scanner utility
- [ ] Config loader implementation
- [ ] Board registry class
- [ ] Logger utility

## Phase 2: RS-485 Management

### High Priority
- [ ] RS485BranchManager class
  - [ ] Multi-threaded branch handling (one thread per branch)
  - [ ] Thread-safe queue for read/write operations
  - [ ] Automatic retry on communication errors
  - [ ] Statistics collection per branch

- [ ] BoardRegistry class
  - [ ] Load configuration from JSON
  - [ ] Board lookup by name/address/branch
  - [ ] Board state tracking (online/offline/error)
  - [ ] Last successful poll timestamp

- [ ] PeriodicPoller class
  - [ ] 5-minute polling cycle
  - [ ] Configurable polling sequence
  - [ ] Skip offline boards
  - [ ] Collect all data into memory buffer

### Medium Priority
- [ ] DataCollector class
  - [ ] Parse raw Modbus data into BoardData structures
  - [ ] Apply calibrations (ADC → physical units)
  - [ ] Detect anomalies (out-of-range values)
  - [ ] Generate alarms

## Phase 2b: Power Supply Control (OWON ODP6033)

Slow control for the OWON ODP6033 (SCPI over TCP, port 3000): CH3 (0-6V/3A)
feeds electronics boards at ~6V, CH1/CH2 (0-60V/3A each) feed SiPM HV
(~55V), mapping is configurable via `config/power_supply.json`.

- [x] `OwonPSU` — low-level SCPI/TCP client (connect with bounded timeout,
      channel select, voltage/current/OVP/OCP setters, measurement queries,
      statistics) — `include/power/owon_psu.h`, `src/power/owon_psu.cpp`
- [x] `PowerSupplyController` — policy layer: applies configured OVP/OCP at
      startup, interlock-gated output enable, ramped HV enable/disable,
      `emergency_shutdown()`, `poll()` — `include/power/power_controller.h`,
      `src/power/power_controller.cpp`
- [x] `PSUConfig`/`PSUChannelConfig`/`PSUChannelStatus` types (`types.h`),
      PSU constants (`constants.h`), `config/power_supply.json` example
- [x] CMakeLists.txt wiring (new sources into `danss_core`, include path)
- [ ] `ConfigLoader::load_power_config()` — real JSON parsing (currently a
      stub returning defaults, same status as `load_system_config()`)
- [ ] Verify against the real OWON ODP6033 on the stand (`d2pwr1.danss.local:3000`):
      `*IDN?`, channel select, voltage/current set, OVP/OCP set, measure
      queries, ramp behavior — see checklist to be run on the stand
- [ ] `power_equipment.cpp` — MIDAS "PowerSupply" Equipment wrapper (Phase 4,
      alongside SlowControl/RunSync — stub in place with the planned interface
      documented in the file)
- [ ] Wire interlock: PowerSupplyController::set_interlock_ready() driven by
      SlowControl's board-online tracking (design TBD — depends on
      BoardRegistry, not yet implemented)
- [ ] Decide/measure real ramp step & interval for SiPM HV (currently
      placeholder defaults: 1V / 500-1000ms) — depends on SiPM bias
      requirements, not yet confirmed with the group

## Phase 3: EEPROM Management

- [ ] EEPROMManager class
  - [ ] Read thermometer IDs (registers 100-147)
  - [ ] Read CPU IDs (registers 148-149)
  - [ ] Parse 1-Wire IDs from register data
  
- [ ] EEPROMValidator class
  - [ ] Load reference data from JSON
  - [ ] Daily verification cycle
  - [ ] Compare current vs reference
  - [ ] Generate mismatch reports
  - [ ] Auto-correction capability (write back correct values)

## Phase 4: MIDAS Integration

### Frontend Structure
- [ ] MIDAS frontend main.cpp
  - [ ] Initialize MIDAS connection
  - [ ] Create equipment instances
  - [ ] Handle run transitions
  
- [ ] SlowControl Equipment
  - [ ] periodic_event() for 5-min polling
  - [ ] read_event() to fill MIDAS banks
  - [ ] Bank structure for board data
  - [ ] ODB integration for settings
  
- [ ] RunSync Equipment
  - [ ] Monitor run state changes
  - [ ] Coordinate Calibration ↔ Physics transitions
  - [ ] Ensure all boards are synchronized

### State Machine
- [ ] RunStateMachine class
  - [ ] States: STOPPED, PHYSICS_RUN, CALIBRATION_RUN, MAINTENANCE
  - [ ] Transition validation
  - [ ] Transition callbacks
  - [ ] Error recovery

### Alarms
- [ ] MIDAS alarm integration
  - [ ] Trip status alarms
  - [ ] Temperature threshold alarms
  - [ ] Communication failure alarms
  - [ ] EEPROM mismatch alarms
  
- [ ] Alarm manager
  - [ ] Rate limiting (prevent alarm storms)
  - [ ] Alarm aggregation (combine similar alarms)
  - [ ] Alarm history

## Phase 5: Advanced Features

### Error Recovery
- [ ] Automatic board reset on persistent errors
- [ ] Board re-initialization after power cycle
- [ ] Graceful degradation (continue with remaining boards)
- [ ] Error log analysis

### Logging & Diagnostics
- [ ] Structured logging (spdlog or similar)
  - [ ] Log levels (DEBUG, INFO, WARNING, ERROR)
  - [ ] Log rotation
  - [ ] Separate logs per branch
  
- [ ] Diagnostics tools
  - [ ] Real-time statistics dashboard
  - [ ] Communication quality metrics
  - [ ] Board health indicators

### Telegram Notifications
- [ ] Telegram bot integration
  - [ ] Critical alarm notifications
  - [ ] Status queries via bot commands
  - [ ] Remote control (start/stop polling)
  
- [ ] Notification manager
  - [ ] Configurable notification rules
  - [ ] User subscription management

### Performance Optimization
- [ ] Batch register reads (read multiple registers in one transaction)
- [ ] Adaptive polling (skip stable boards more often)
- [ ] Connection pooling (keep serial ports open)
- [ ] Memory pool for BoardData structures

### Web Interface (Optional)
- [ ] REST API for status queries
- [ ] Real-time WebSocket updates
- [ ] Board configuration editor
- [ ] Historical data viewer

## Phase 6: Testing

### Unit Tests
- [ ] ModbusRTU tests
  - [ ] CRC calculation
  - [ ] Frame building
  - [ ] Response parsing
  
- [ ] Configuration loader tests
- [ ] Board registry tests
- [ ] State machine tests

### Integration Tests
- [ ] Modbus simulator (fake boards)
- [ ] End-to-end polling test
- [ ] EEPROM validation test
- [ ] State transition test

### Hardware Tests
- [ ] Single board communication
- [ ] Full branch (60 boards) stress test
- [ ] All branches (240 boards) stress test
- [ ] Long-duration stability test (24+ hours)

## Documentation

- [x] README.md
- [x] BUILD.md
- [ ] ARCHITECTURE.md (system design document)
- [ ] API.md (class interfaces)
- [ ] PROTOCOL.md (Modbus register map details)
- [ ] OPERATIONS.md (run procedures, troubleshooting)
- [ ] Doxygen configuration
- [ ] Code comments

## Code Quality

- [ ] clang-format configuration
- [ ] clang-tidy checks
- [ ] Address sanitizer testing
- [ ] Thread sanitizer testing
- [ ] Valgrind memory leak checks
- [ ] Static analysis (cppcheck)

## Deployment

- [ ] systemd service file
- [ ] Startup/shutdown scripts
- [ ] Log rotation configuration
- [ ] Monitoring integration (Nagios/Prometheus)
- [ ] Backup scripts for configuration

## Future Enhancements

- [ ] Support for different board firmware versions
- [ ] Hot-swap board replacement
- [ ] Remote firmware updates
- [ ] Machine learning for anomaly detection
- [ ] Automatic calibration drift compensation
- [ ] Historical data analysis tools

---

## Priority Order

1. **Immediate (this week)**
   - Config loader
   - Board registry
   - RS485 manager skeleton

2. **Short-term (this month)**
   - Periodic poller
   - EEPROM manager
   - Basic MIDAS frontend

3. **Medium-term (this quarter)**
   - Full MIDAS integration
   - State machine
   - Alarms

4. **Long-term (ongoing)**
   - Performance optimization
   - Advanced features
   - Documentation
