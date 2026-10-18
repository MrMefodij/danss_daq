# Modbus Scanner Output Examples

## Standard Mode (No Options)

```bash
$ ./modbus_scanner /dev/ttyUSB0 -s 1 -e 30
```

Output:
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

Starting scan...

[==================================================] 100% (30/30)
```

## Show Addresses Mode (--show-addresses)

```bash
$ ./modbus_scanner --show-addresses /dev/ttyUSB0 -s 1 -e 30
```

Output:
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

Starting scan...

[===================>                              ]  40% (12/30) Address:  12   
```

**Benefits:**
- See which address is currently being scanned
- Useful for debugging slow responses
- Progress bar updates in real-time
- Compact single-line display

## Verbose Mode (-v)

```bash
$ ./modbus_scanner -v /dev/ttyUSB0 -s 1 -e 10
```

Output:
```
╔════════════════════════════════════════════════════════════╗
║         DANSS Modbus Address Scanner                      ║
║         Scan RS-485 bus for active boards                 ║
╚════════════════════════════════════════════════════════════╝

Scan parameters:
  Device: /dev/ttyUSB0
  Baud rate: 19200
  Address range: 1 - 10
  Timeout: 500 ms
  Delay between scans: 100 ms
  Mode: Full scan

Starting scan...

Scanning address   1... ✓ FOUND (update #: 0, response: 45.3 ms)
Scanning address   2... ✓ FOUND (update #: 123, response: 42.1 ms)
Scanning address   3... ✗ No response (Timeout waiting for response)
Scanning address   4... ✗ No response (Timeout waiting for response)
Scanning address   5... ✓ FOUND (update #: 456, response: 43.5 ms)
Scanning address   6... ✗ No response (Timeout waiting for response)
Scanning address   7... ✗ No response (Timeout waiting for response)
Scanning address   8... ✗ No response (Timeout waiting for response)
Scanning address   9... ✗ No response (Timeout waiting for response)
Scanning address  10... ✓ FOUND (update #: 789, response: 44.2 ms)
```

**Benefits:**
- See every address attempt
- See exact error messages
- See response times for each board
- Best for troubleshooting

## Mode Comparison

| Mode | Command | Use Case | Speed |
|------|---------|----------|-------|
| **Standard** | `./modbus_scanner /dev/ttyUSB0` | Normal scanning | Fast |
| **Show Addresses** | `./modbus_scanner --show-addresses /dev/ttyUSB0` | Monitor progress | Fast |
| **Verbose** | `./modbus_scanner -v /dev/ttyUSB0` | Troubleshooting | Medium |
| **Quick** | `./modbus_scanner -q /dev/ttyUSB0` | Fast discovery | Fastest |

## Recommended Usage

### Initial Setup
```bash
# First scan to see what's there
./modbus_scanner --show-addresses /dev/ttyUSB0
```

### Troubleshooting Missing Boards
```bash
# Detailed info for each address
./modbus_scanner -v -s 1 -e 30 /dev/ttyUSB0
```

### Production Scanning (All Branches)
```bash
# Fast scan of all branches
for i in {0..7}; do
  echo "Scanning branch $i..."
  ./modbus_scanner -s 1 -e 30 /dev/ttyUSB$i > branch${i}.txt
done
```

### Testing Single Board
```bash
# Detailed scan of single address
./modbus_scanner -v -s 5 -e 5 /dev/ttyUSB0
```

## Output Interpretation

### Progress Bar Elements

Standard mode:
```
[==================================================] 100% (30/30)
 │                                                │   │    └─ Current/Total
 │                                                │   └─ Percentage
 │                                                └─ Progress indicator
 └─ Progress bar (50 characters)
```

Show addresses mode:
```
[===================>                              ]  40% (12/30) Address:  12
 │                                                │   │    │      └─ Current address
 │                                                │   │    └─ Current/Total
 │                                                │   └─ Percentage
 └─ Progress bar (50 characters)
```

### Verbose Output Symbols

- `✓ FOUND` - Board responded successfully
- `✗ No response` - Board did not respond (timeout or error)

### Common Error Messages

- `Timeout waiting for response` - Board not responding (check power, address)
- `CRC error in response` - Communication issue (check cables, termination)
- `Failed to open serial port` - Device not accessible (check permissions)
- `Write error` - Cannot send to serial port (hardware issue)

## Tips

1. **Use `--show-addresses` by default** - it's helpful to see progress without being too verbose

2. **Switch to `-v` when troubleshooting** - see detailed error messages

3. **Use `-q` for quick checks** - when you just need to know which addresses respond

4. **Redirect output for records**:
   ```bash
   ./modbus_scanner -v /dev/ttyUSB0 > scan_$(date +%Y%m%d_%H%M%S).txt
   ```

5. **Combine with grep for filtering**:
   ```bash
   ./modbus_scanner -v /dev/ttyUSB0 | grep "FOUND"
   ```
