# Build Instructions

## Prerequisites

### 1. Install MIDAS 2.1

```bash
# Download MIDAS
cd /opt
git clone https://github.com/midas-daqsystem/midas.git
cd midas

# Checkout MIDAS 2.1
git checkout midas-2.1

# Build
mkdir build
cd build
cmake ..
make -j$(nproc)
make install

# Set environment variables (add to ~/.bashrc)
export MIDASSYS=/opt/midas
export MIDAS_EXPTAB=/etc/exptab
export PATH=$MIDASSYS/bin:$PATH
export LD_LIBRARY_PATH=$MIDASSYS/lib:$LD_LIBRARY_PATH
```

### 2. Install Boost Libraries

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install libboost-all-dev
```

**CentOS/RHEL:**
```bash
sudo yum install boost-devel
```

**From source:**
```bash
wget https://boostorg.jfrog.io/artifactory/main/release/1.81.0/source/boost_1_81_0.tar.gz
tar xzf boost_1_81_0.tar.gz
cd boost_1_81_0
./bootstrap.sh --prefix=/usr/local
./b2 install
```

### 3. Install Build Tools

```bash
# Ubuntu/Debian
sudo apt-get install build-essential cmake git

# CentOS/RHEL
sudo yum install gcc gcc-c++ cmake git
```

## Building the Project

### 1. Clone Repository

```bash
git clone <repository-url> danss-slow-control
cd danss-slow-control
```

### 2. Configure with CMake

```bash
mkdir build
cd build

# Standard build
cmake ..

# Or with specific options
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DBUILD_TESTS=ON \
  ..
```

### 3. Build

```bash
# Build all targets
make -j$(nproc)

# Or build specific target
make danss_frontend
make danss_core
```

### 4. Install (optional)

```bash
sudo make install
```

This will install:
- `danss_frontend` binary to `/usr/local/bin/`
- Configuration files to `/usr/local/etc/danss/`

## Configuration

### 1. Generate Full Board Configuration

For 240 boards across 8 RS-485 branches:

```bash
cd config

# Generate board map
python3 generate_full_config.py > board_map_full.json

# Generate EEPROM reference
python3 generate_full_config.py --eeprom > eeprom_reference_full.json
```

### 2. Edit Configuration

Edit `config/board_map.json` or `config/board_map_full.json`:

- Verify serial port device paths (`/dev/ttyUSB0-7`)
- Adjust baud rates if needed
- Enable/disable specific boards
- Adjust polling intervals

### 3. Setup Serial Ports

```bash
# Check available USB-RS485 adapters
ls -l /dev/ttyUSB*

# Add user to dialout group (for serial port access)
sudo usermod -a -G dialout $USER

# Set permissions (if needed)
sudo chmod 666 /dev/ttyUSB*

# For persistent permissions, create udev rule:
sudo nano /etc/udev/rules.d/99-usb-serial.rules
# Add:
# SUBSYSTEM=="tty", ATTRS{idVendor}=="xxxx", MODE="0666"

# Reload udev
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## Testing

### 1. Test Modbus Communication

Create a simple test program to verify RS-485 communication:

```bash
cd build
./bin/danss_frontend --test-modbus /dev/ttyUSB0 19200
```

### 2. Run Unit Tests (if enabled)

```bash
cd build
ctest --output-on-failure
```

### 3. Test Frontend Without MIDAS

```bash
./bin/danss_frontend --standalone
```

## Troubleshooting

### CMake can't find MIDAS

```bash
# Make sure MIDASSYS is set
echo $MIDASSYS

# If not set:
export MIDASSYS=/opt/midas
export MIDAS_EXPTAB=/etc/exptab
```

### CMake can't find Boost

```bash
# Specify Boost root manually
cmake -DBOOST_ROOT=/usr/local ..
```

### Serial port permission denied

```bash
# Check permissions
ls -l /dev/ttyUSB0

# Add user to dialout group
sudo usermod -a -G dialout $USER

# Log out and log back in for changes to take effect
```

### Modbus timeout errors

- Check baud rate matches board settings
- Verify cable connections
- Check for RS-485 termination resistors
- Try increasing timeout in `board_map.json`:
  ```json
  "modbus_timeout_ms": 2000,
  "response_timeout_ms": 1000
  ```

### Build errors with old GCC

The project requires C++17. Upgrade GCC or use a newer compiler:

```bash
# Check GCC version
gcc --version

# Ubuntu: install newer GCC
sudo apt-get install gcc-9 g++-9
export CC=gcc-9
export CXX=g++-9
cmake ..
```

## Development Build

For development with debug symbols and sanitizers:

```bash
mkdir build-debug
cd build-debug

cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=undefined" \
  ..

make -j$(nproc)
```

## Cross-Compilation (for embedded targets)

Example for ARM target:

```bash
mkdir build-arm
cd build-arm

cmake \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  ..

make -j$(nproc)
```

## Performance Profiling

```bash
# Build with profiling enabled
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make

# Run with profiler
perf record -g ./bin/danss_frontend
perf report
```

## Next Steps

After successful build:

1. Configure your MIDAS experiment
2. Test serial communication with boards
3. Run frontend in test mode
4. Integrate with MIDAS DAQ
5. Set up monitoring and alarms

See [README.md](README.md) for runtime instructions.
