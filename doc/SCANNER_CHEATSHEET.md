# Modbus Scanner - Quick Reference

## Common Commands

```bash
# Basic scan (all addresses)
./modbus_scanner /dev/ttyUSB0

# Show current address being checked
./modbus_scanner --show-addresses /dev/ttyUSB0

# Scan specific range (fast)
./modbus_scanner -s 1 -e 30 /dev/ttyUSB0

# Verbose mode (see all attempts)
./modbus_scanner -v /dev/ttyUSB0

# Quick scan (less info, faster)
./modbus_scanner -q -s 1 -e 30 /dev/ttyUSB0

# Different baud rate
./modbus_scanner /dev/ttyUSB0 9600

# Longer timeout for slow boards
./modbus_scanner -t 2000 /dev/ttyUSB0

# Scan all branches
for i in {0..7}; do
  ./modbus_scanner /dev/ttyUSB$i > branch${i}.txt
done
```

## Options

| Short | Long | Description | Default |
|-------|------|-------------|---------|
| `-s` | `--start` | Start address (1-247) | 1 |
| `-e` | `--end` | End address (1-247) | 247 |
| `-t` | `--timeout` | Timeout in ms | 500 |
| `-v` | `--verbose` | Show all attempts | off |
| `-q` | `--quick` | Quick scan mode | off |
| | `--show-addresses` | Show current address | off |
| | `--delay` | Delay between scans (ms) | 100 |
| `-h` | `--help` | Show help | - |

## Typical Workflows

### Initial Setup
```bash
# 1. Scan to find boards
./modbus_scanner /dev/ttyUSB0 > scan.txt

# 2. Review results
cat scan.txt

# 3. Copy suggested JSON config to board_map.json
# 4. Edit board names (PMT-01-01, etc.)
```

### Troubleshooting
```bash
# Find missing boards
./modbus_scanner -v /dev/ttyUSB0

# Test different baud rates
for baud in 9600 19200 38400; do
  echo "Trying $baud..."
  ./modbus_scanner /dev/ttyUSB0 $baud
done
```

### Performance Testing
```bash
# Stress test with minimal delay
./modbus_scanner --delay 10 -s 1 -e 60 /dev/ttyUSB0

# Check for CRC errors (indicates noise)
```

## Interpreting Results

✅ **Good:**
- Response time 30-100 ms
- No CRC errors
- All expected boards found

⚠️ **Warning:**
- Response time > 100 ms → check cables
- Occasional CRC errors → check termination

❌ **Critical:**
- Many CRC errors → bad cable/termination
- All timeouts → wrong baud/wiring

## Common Issues

**No boards found:**
- Check A/B wiring
- Try different baud rates
- Verify termination (120Ω)
- Check power to boards

**CRC errors:**
- Add/check termination resistors
- Use shielded cable
- Reduce cable length
- Move away from noise sources

**Permission denied:**
```bash
sudo usermod -a -G dialout $USER
# Log out and back in
```

## See Full Documentation

[doc/MODBUS_SCANNER.md](MODBUS_SCANNER.md)
