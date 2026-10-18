# Modbus Scanner Utility

## Overview

`modbus_scanner` is a utility tool for discovering and identifying Modbus RTU devices on an RS-485 bus. It scans a range of addresses and reports which boards are present and responsive.

## Purpose

- **Initial setup**: Discover all boards on a new RS-485 branch
- **Troubleshooting**: Identify which boards are offline
- **Configuration**: Generate board_map.json configuration automatically
- **Testing**: Verify RS-485 wiring and termination

## Usage

### Basic Scan

```bash
# Scan all addresses (1-247) on /dev/ttyUSB0 at 19200 baud
./modbus_scanner /dev/ttyUSB0

# Scan with different baud rate
./modbus_scanner /dev/ttyUSB0 9600
```

### Scan Specific Range

```bash
# Scan only addresses 1-30
./modbus_scanner -s 1 -e 30 /dev/ttyUSB0

# Quick scan (faster, less information)
./modbus_scanner -s 1 -e 30 --quick /dev/ttyUSB0
```

### Advanced Options

```bash
# Verbose mode - show all attempts
./modbus_scanner -v /dev/ttyUSB0

# Show current address being scanned (compact mode)
./modbus_scanner --show-addresses /dev/ttyUSB0

# Longer timeout for slow boards
./modbus_scanner -t 2000 /dev/ttyUSB0

# Faster scan with reduced delay
./modbus_scanner --delay 50 /dev/ttyUSB0
```

## Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-h, --help` | Show help message | - |
| `-s, --start <addr>` | Start address (1-247) | 1 |
| `-e, --end <addr>` | End address (1-247) | 247 |
| `-t, --timeout <ms>` | Timeout per address | 500 ms |
| `-v, --verbose` | Show all scan attempts | Off |
| `-q, --quick` | Quick scan mode | Off |
| `--show-addresses` | Show current address in progress bar | Off |
| `--delay <ms>` | Delay between scans | 100 ms |

## Output Example

### Successful Scan

```
╔════════════════════════════════════════════════════════════╗
║         DANSS Modbus Address Scanner                      ║
║         Scan RS-485 bus for active boards                 ║
╚════════════════════════════════════════════════════════════╝

Scan parameters:
  Device: /dev/ttyUSB0
  Baud rate: 19200
  Address range: 1 - 30
  Timeout: 500 ms
  Delay between scans: 100 ms
  Mode: Full scan

[==================================================] 100% (30/30)

╔════════════════════════════════════════════════════════════╗
║                    Scan Results                           ║
╚════════════════════════════════════════════════════════════╝

Total addresses scanned: 30
Active boards found: 5

Active boards:
------------------------------------------------------------------------------
 Address  Update#    HV st     Trip    TripEn     Gain  Response (ms)
------------------------------------------------------------------------------
       1     1234   0x0fff        -     0,1,2        -          45.30
       2      877   0x1fff        0     0,1,2        -          42.10
     247        5   0x0000        -         -        -          43.50  <- factory address (new board)
------------------------------------------------------------------------------
Trip / TripEn / Gain: analog boards 0-2 with the bit set ("-" = none)
Board at address 247 has the factory address: assign a real address before use.

(В полном режиме дальше идёт таблица по каналам каждой платы: аналоговая плата,
ток, температура, HV on/off и температура CPU — всё из input 0–27, карта v2.)

Suggested board_map.json configuration:
{
  "boards": [
    {
      "address": 1,
      "name": "PMT-XX-01",
      "enabled": true
    },
    {
      "address": 2,
      "name": "PMT-XX-02",
      "enabled": true
    },
    {
      "address": 3,
      "name": "PMT-XX-03",
      "enabled": true
    },
    {
      "address": 4,
      "name": "PMT-XX-04",
      "enabled": true
    },
    {
      "address": 5,
      "name": "PMT-XX-05",
      "enabled": true
    }
  ]
}

Scan completed in 15.32 seconds

Communication statistics:
  Total requests: 30
  Successful: 5
  Timeouts: 25
  CRC errors: 0
  Average response time: 44.18 ms
```

### Verbose Mode

```bash
./modbus_scanner -v -s 1 -e 10 /dev/ttyUSB0
```

Output:
```
Scanning address 1... ✓ FOUND (update #: 0, response: 45.3 ms)
Scanning address 2... ✓ FOUND (update #: 123, response: 42.1 ms)
Scanning address 3... ✗ No response
Scanning address 4... ✗ No response
Scanning address 5... ✓ FOUND (update #: 789, response: 44.2 ms)
...

Non-responsive addresses:
  Address   3: Timeout waiting for response
  Address   4: Timeout waiting for response
  Address   6: Timeout waiting for response
  ...
```

## Use Cases

### 1. Initial System Setup

When setting up a new RS-485 branch:

```bash
# Scan to discover all boards
./modbus_scanner /dev/ttyUSB0 > scan_results.txt

# Copy suggested configuration to board_map.json
# Edit board names (PMT-01-01, PMT-01-02, etc.)
```

### 2. Troubleshooting Offline Boards

If some boards are not responding:

```bash
# Scan the problematic branch
./modbus_scanner -v /dev/ttyUSB3

# Check which specific addresses are missing
# Verify power, wiring, address switches
```

### 3. Testing After Hardware Changes

After rewiring or adding new boards:

```bash
# Quick scan to verify all boards are present
./modbus_scanner -s 1 -e 30 --quick /dev/ttyUSB0

# Compare with expected configuration
```

### 4. Performance Testing

Test RS-485 bus performance:

```bash
# Scan with minimal delay to stress test
./modbus_scanner --delay 10 -s 1 -e 60 /dev/ttyUSB0

# Check statistics for errors
# CRC errors indicate noise/interference
# High timeouts indicate bus loading
```

## Interpreting Results

### Good Results
- All expected boards respond
- Response times 30-100 ms
- No CRC errors
- No timeouts (except for unused addresses)

### Warning Signs
- Response times > 100 ms
  → Check baud rate, cable length
  
- Occasional CRC errors
  → Check termination resistors
  → Check for EMI/noise sources
  
- Some boards timeout
  → Verify power to those boards
  → Check address switches
  → Verify cables

### Critical Issues
- Many CRC errors (>10%)
  → Bad cable or termination
  → EMI interference
  
- All boards timeout
  → Wrong baud rate
  → Wrong serial port
  → RS-485 adapter not working
  → Reversed A/B wiring

## Tips for Best Results

### 1. Baud Rate
Make sure the baud rate matches the board firmware:
```bash
# Try common baud rates if unsure
./modbus_scanner /dev/ttyUSB0 9600
./modbus_scanner /dev/ttyUSB0 19200
./modbus_scanner /dev/ttyUSB0 38400
```

### 2. Timeout
Increase timeout for long cables or slow boards:
```bash
# Use longer timeout (2 seconds)
./modbus_scanner -t 2000 /dev/ttyUSB0
```

### 3. Scan Speed
Adjust delay based on bus loading:
```bash
# Faster for lightly loaded bus
./modbus_scanner --delay 50 /dev/ttyUSB0

# Slower for heavily loaded bus or long cables
./modbus_scanner --delay 200 /dev/ttyUSB0
```

### 4. Targeted Scanning
Scan only expected range to save time:
```bash
# If you know boards are at addresses 1-30
./modbus_scanner -s 1 -e 30 /dev/ttyUSB0
```

## Integration with Configuration

### Generate Configuration

After scanning, you can use the suggested configuration:

1. Run scanner and save output:
```bash
./modbus_scanner /dev/ttyUSB0 > branch0_scan.txt
```

2. Extract the JSON section

3. Edit board names to match your naming scheme:
```json
{
  "address": 1,
  "name": "PMT-01-01",  // Change from PMT-XX-01
  "enabled": true
}
```

4. Add to your `board_map.json`

### Automated Script

You can create a script to scan all branches:

```bash
#!/bin/bash
for i in {0..7}; do
    echo "Scanning branch $i..."
    ./modbus_scanner -s 1 -e 30 /dev/ttyUSB$i > branch${i}_scan.txt
done
```

## Troubleshooting

### Scanner Won't Start

**Error: Failed to open serial port: Permission denied**
```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER
# Log out and back in

# Or set permissions (temporary)
sudo chmod 666 /dev/ttyUSB0
```

**Error: Failed to open serial port: No such file**
```bash
# Check available ports
ls -l /dev/ttyUSB*
ls -l /dev/ttyS*

# Use correct device
```

### No Boards Found

1. **Check wiring**
   - A and B correctly connected
   - Common ground connected
   - Termination resistors (120Ω) at both ends

2. **Try different baud rates**
   ```bash
   ./modbus_scanner /dev/ttyUSB0 9600
   ./modbus_scanner /dev/ttyUSB0 19200
   ./modbus_scanner /dev/ttyUSB0 115200
   ```

3. **Increase timeout**
   ```bash
   ./modbus_scanner -t 2000 /dev/ttyUSB0
   ```

4. **Test with known board**
   - Manually set one board to address 1
   - Scan only that address
   ```bash
   ./modbus_scanner -s 1 -e 1 -v /dev/ttyUSB0
   ```

### Many CRC Errors

1. Check termination resistors (120Ω at each end)
2. Reduce cable length if possible
3. Use shielded twisted pair cable
4. Move away from noise sources (motors, fluorescent lights)
5. Reduce scan speed:
   ```bash
   ./modbus_scanner --delay 200 /dev/ttyUSB0
   ```

## Example Workflow

### Setting Up a New Branch

```bash
# Step 1: Verify USB adapter is detected
ls -l /dev/ttyUSB*

# Step 2: Test communication with one board (address 1)
./modbus_scanner -s 1 -e 1 -v /dev/ttyUSB0

# Step 3: If successful, scan full range
./modbus_scanner -s 1 -e 30 /dev/ttyUSB0 > branch0_scan.txt

# Step 4: Review results
cat branch0_scan.txt

# Step 5: Extract JSON configuration
# Copy the suggested configuration to board_map.json

# Step 6: Test with main program
./danss_frontend --test-modbus /dev/ttyUSB0 19200
```

## See Also

- [QUICKSTART.md](../QUICKSTART.md) - Getting started guide
- [BUILD.md](../doc/BUILD.md) - Build instructions
- [board_map.json](../config/board_map.json) - Configuration example
