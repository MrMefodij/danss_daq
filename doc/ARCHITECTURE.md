# System Architecture

## Overview

DANSS Slow Control is a multi-threaded, event-driven system for monitoring ~240 detector boards across 4-8 RS-485 branches.

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      MIDAS DAQ System                       │
│  ┌────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │ Data Logger│  │  Web Monitor │  │  Alarm System    │   │
│  └─────┬──────┘  └──────┬───────┘  └────────┬─────────┘   │
└────────┼─────────────────┼──────────────────┼──────────────┘
         │                 │                  │
         │            ODB (Online Database)   │
         │                 │                  │
    ┌────▼─────────────────▼──────────────────▼────┐
    │         DANSS Frontend Process                │
    │  ┌──────────────────────────────────────┐    │
    │  │  SlowControl Equipment               │    │
    │  │  - Read event every 5 minutes        │    │
    │  │  - Fill MIDAS banks with board data  │    │
    │  └────────┬─────────────────────────────┘    │
    │           │                                   │
    │  ┌────────▼─────────────────────────────┐    │
    │  │  RS485 Branch Manager                │    │
    │  │  - Manages 4-8 serial port threads   │    │
    │  │  - Coordinates polling cycles         │    │
    │  │  - Handles retries and errors         │    │
    │  └────────┬─────────────────────────────┘    │
    │           │                                   │
    │  ┌────────▼─────────────────────────────┐    │
    │  │  Board Registry                      │    │
    │  │  - 240 board configurations          │    │
    │  │  - EEPROM reference data             │    │
    │  │  - Board state tracking              │    │
    │  └──────────────────────────────────────┘    │
    └───────────────────────────────────────────────┘
                         │
         ┌───────────────┼───────────────┐
         │               │               │
    ┌────▼────┐     ┌───▼────┐     ┌───▼────┐
    │Thread 0 │     │Thread 1│ ... │Thread 7│
    │/ttyUSB0 │     │/ttyUSB1│     │/ttyUSB7│
    └────┬────┘     └───┬────┘     └───┬────┘
         │              │              │
    ┌────▼────┐    ┌───▼────┐    ┌───▼────┐
    │ Modbus  │    │ Modbus │    │ Modbus │
    │   RTU   │    │   RTU  │    │   RTU  │
    └────┬────┘    └───┬────┘    └───┬────┘
         │             │              │
    ┌────▼─────────────▼──────────────▼────┐
    │         RS-485 Network                │
    │  ~30 boards per branch (240 total)   │
    │  Each board: 12 SiPM channels        │
    │              96 DAC channels          │
    │              Modbus slave address     │
    └───────────────────────────────────────┘
```

## Component Details

### 1. MIDAS Frontend

**Responsibility**: Interface with MIDAS DAQ system

**Key Classes**:
- `SlowControlEquipment` - Periodic read events (5 min)
- `RunSyncEquipment` - Handle run transitions

**Data Flow**:
1. MIDAS calls `read_event()` every 5 minutes
2. Frontend queries RS485Manager for latest data
3. Data packed into MIDAS banks
4. Event written to MIDAS buffer

### 2. RS485 Branch Manager

**Responsibility**: Coordinate multi-branch polling

**Architecture**:
```
RS485BranchManager
├── io_context_pool_[8]       # One per branch
├── modbus_instances_[8]      # ModbusRTU per branch
├── worker_threads_[8]        # One thread per branch
└── board_data_buffer_        # Shared data structure
```

**Threading Model**:
- **Main thread**: Coordinates polling cycles
- **Worker threads**: Each handles one RS-485 branch
- **Synchronization**: Lock-free queues for data collection

**Polling Cycle**:
```
Every 5 minutes:
  For each branch (parallel):
    For each board on branch (sequential):
      1. Read input registers (currents, temps, trip)
      2. Read holding registers (DAC, CSR, counters)
      3. Store in buffer
      4. Check thresholds → generate alarms
  
  Wait for all branches to complete
  Notify MIDAS equipment (data ready)
```

### 3. Modbus RTU Layer

**Responsibility**: Low-level Modbus communication

**Implementation**:
- Boost.Asio serial port
- CRC16 calculation
- Timeout handling
- Retry logic

**Transaction Flow**:
```
Client Request:
  [Slave Addr][Function Code][Start Reg][Count][CRC16]
  
Board Response:
  [Slave Addr][Function Code][Byte Count][Data...][CRC16]
  
Timing:
  ├─ T3.5 char times (silence before request)
  ├─ Send request
  ├─ Wait for response (timeout: 1000ms)
  ├─ Verify CRC
  └─ Parse data
```

### 4. EEPROM Management

**Responsibility**: Validate and maintain board configuration

**Daily Cycle**:
```
Once per day (typically at 3 AM):
  For each board:
    1. Read thermometer IDs (registers 100-147)
    2. Read CPU ID (registers 148-149)
    3. Compare with reference data
    4. If mismatch:
       a. Log alarm
       b. Optionally write correct values back
       c. Notify operators
```

**Data Structure**:
```
EEPROM Reference (JSON):
{
  "boards": [
    {
      "name": "PMT-01-01",
      "thermometer_ids": ["28FF...", "28FF...", ...], // 12 IDs
      "cpu_id": "ABC123DEF456"
    }
  ]
}
```

## Data Structures

### BoardData (in-memory)

```cpp
struct BoardData {
  // Identification
  string board_name;
  uint8_t modbus_address;
  
  // Input registers (map v2, doc/MODBUS_MAP_v2.md)
  uint16_t update_counter;      // 0
  uint16_t hv_status;           // 1: trip per analog board + HV per channel
  uint16_t gain_trip_status;    // 2
  array<uint16_t, 12> sipm_currents;      // 3-14
  array<uint16_t, 12> sipm_temperatures;  // 15-26
  uint16_t cpu_temperature;     // 27
  array<uint16_t, 96> dac_readback;       // 150-245
  
  // Holding registers
  array<uint16_t, 96> dac_settings;       // 0-95
  uint16_t hv_enable;           // 96
  uint16_t gain_trip_enable;    // 97
  uint16_t global_csr;          // 98
  uint16_t cycle_delay;         // 99
  
  // Metadata
  uint64_t timestamp_ms;
  bool data_valid;
};
```

### MIDAS Bank Structure

```
Event ID: 1 (Slow Control)
Bank: BDAT (Board Data)
  - Board count (uint32)
  - For each board:
    - Name (32 chars)
    - Address (uint8)
    - Currents (12 × uint16)
    - Temperatures (12 × uint16)
    - DAC settings (96 × uint16)
    - Flags (uint16)
    - Timestamp (uint64)
```

## Timing Analysis

### Single Board Poll Time

```
Read operation:
  - Request transmission: ~5ms (at 19200 baud)
  - Board processing: ~10ms
  - Response transmission: ~20ms (typical)
  - Total per transaction: ~35ms

Typical board poll (2 read transactions):
  - Read input registers 0-27 (28 regs): ~35ms
  - Read holding registers 0-100 (101 regs): ~50ms
  - Total: ~85ms per board
```

### Full System Poll Time

```
Per branch (30 boards):
  - Sequential polling: 30 × 85ms = 2.55 seconds
  - Add overhead (10%): 2.8 seconds

All 8 branches (parallel):
  - Maximum branch time: 2.8 seconds
  - Margin: 300s / 2.8s = 107x (plenty of headroom)
```

### EEPROM Check Time

```
Per board EEPROM read:
  - 12 thermometer IDs (48 regs): ~60ms
  - CPU ID (2 regs): ~35ms
  - Total: ~95ms per board

All 240 boards (parallel across branches):
  - Per branch (30 boards): 30 × 95ms = 2.85s
  - Total: < 3 seconds
```

## State Machine

### Run States

```
┌──────────┐
│ STOPPED  │ ◄──────────┐
└────┬─────┘            │
     │                  │
     │ start run        │ stop
     ▼                  │
┌──────────┐            │
│ PHYSICS  │────────────┤
│   RUN    │            │
└────┬─────┘            │
     │                  │
     │ calibrate        │
     ▼                  │
┌──────────┐            │
│CALIBRATION──────────┤
│   RUN    │            │
└──────────┘            │
     │                  │
     │ resume           │
     └──────────────────┘
```

### Transition Rules

1. **STOPPED → PHYSICS**
   - Verify all boards online
   - Load DAC settings
   - Enable HV

2. **PHYSICS → CALIBRATION**
   - Wait for run pause
   - Change DAC to calibration values
   - Signal LED pulser

3. **CALIBRATION → PHYSICS**
   - Restore physics DAC values
   - Disable LED pulser
   - Resume run

4. **ANY → STOPPED**
   - Disable HV
   - Save current state
   - Close communications

## Error Handling

### Communication Errors

```
Modbus timeout:
  1. Retry (up to 3 times)
  2. If still failing:
     a. Mark board offline
     b. Generate alarm
     c. Continue with other boards
  3. On next cycle, try again

CRC error:
  1. Immediate retry
  2. Log error count
  3. If persistent (>10 in row):
     - Suspect cable/interference
     - Generate alarm

Serial port error:
  1. Close and reopen port
  2. Retry communication
  3. If unsuccessful:
     - Disable branch
     - Generate critical alarm
```

### Board Anomalies

```
Trip detected:
  - Immediate alarm (CRITICAL)
  - Log which SiPM channel(s)
  - Disable affected board (optional)

Temperature out of range:
  - Warning alarm (WARNING)
  - Monitor trend
  - If increasing: escalate to ERROR

Current out of range:
  - Log anomaly
  - Check if persistent
  - Alarm if >5 cycles

EEPROM mismatch:
  - ERROR alarm
  - Attempt auto-correction
  - Require operator verification
```

## Performance Considerations

### Memory Usage

```
Per board data: ~300 bytes
240 boards: 72 KB
Double buffering: 144 KB
Negligible for modern systems
```

### CPU Usage

```
Modbus processing: <1% per branch
8 threads: <10% total
MIDAS overhead: <5%
Plenty of headroom
```

### Thread Safety

```
Shared resources:
  - Board data buffer (read-write lock)
  - Configuration (read-only after init)
  - Alarm queue (mutex-protected)

Lock-free design:
  - Each thread has private Modbus instance
  - Data collection uses atomic operations
  - Minimal contention
```

## Deployment Architecture

```
┌─────────────────────────────────────┐
│      DANSS DAQ Computer             │
│  ┌──────────────────────────────┐   │
│  │  MIDAS System                │   │
│  │  - mserver                   │   │
│  │  - mlogger                   │   │
│  │  - mhttpd (web interface)    │   │
│  └──────────────────────────────┘   │
│                                     │
│  ┌──────────────────────────────┐   │
│  │  danss_frontend              │   │
│  │  (this project)              │   │
│  └────────┬─────────────────────┘   │
│           │                         │
│  ┌────────▼─────────────────────┐   │
│  │  USB Hub (8× USB-RS485)      │   │
│  └────────┬─────────────────────┘   │
└───────────┼──────────────────────────┘
            │
            │ RS-485 cables (shielded, twisted pair)
            │
    ┌───────▼────────┐
    │  Junction Box  │ (optional)
    │  with surge    │
    │  protection    │
    └───────┬────────┘
            │
    ┌───────┴────────┐
    │                │
┌───▼───┐        ┌──▼────┐
│Branch │        │Branch │  ...
│  0    │        │  1    │
│(30 bds)        │(30 bds)
└───────┘        └───────┘
```

## Future Enhancements

1. **Web Dashboard** - Real-time monitoring via websockets
2. **Machine Learning** - Anomaly detection from historical data
3. **Distributed Architecture** - Scale to >1000 boards
4. **Hot-swap Support** - Replace boards without stopping
5. **Automatic Calibration** - Drift compensation using ML

---

For implementation details, see source code comments and individual class documentation.
