#include "common/types.h"
#include "common/constants.h"
#include "common/logger.h"
#include "modbus/modbus_rtu.h"
#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <chrono>

using namespace danss;

void print_banner() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║   DANSS Slow Control & DAQ System                         ║\n";
    std::cout << "║   MIDAS 2.1 Frontend                                      ║\n";
    std::cout << "║   Version 1.0.0                                           ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
}

void print_help() {
    std::cout << "Usage: danss_frontend [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -e, --experiment <name> MIDAS experiment name\n";
    std::cout << "  -c, --config <file>     Configuration file (default: config/board_map.json)\n";
    std::cout << "  -t, --test-modbus <dev> <baud> [addr]  Test Modbus communication\n";
    std::cout << "                          addr: board address to test (default: 1)\n";
    std::cout << "  --standalone            Run without MIDAS (testing mode)\n";
    std::cout << "  -v, --verbose           Enable verbose logging\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  danss_frontend -e DANSS\n";
    std::cout << "  danss_frontend --test-modbus /dev/ttyUSB0 19200\n";
    std::cout << "  danss_frontend --test-modbus /dev/ttyUSB0 19200 5\n";
    std::cout << "  danss_frontend --standalone -v\n";
    std::cout << "\n";
    std::cout << "For scanning multiple addresses, use modbus_scanner:\n";
    std::cout << "  modbus_scanner --show-addresses /dev/ttyUSB0\n";
    std::cout << "\n";
}

bool test_modbus_communication(const std::string& device, uint32_t baud_rate, uint8_t address = 1) {
    LOG_INFO("Testing Modbus communication on " + device);
    
    auto modbus = std::make_unique<ModbusRTU>(device, baud_rate);
    
    if (!modbus->open()) {
        LOG_ERROR("Failed to open serial port: " + modbus->get_last_error());
        return false;
    }
    
    LOG_INFO("Serial port opened successfully");
    
    // Try to read from board at specified address
    std::cout << "\nTrying to communicate with board at address " << static_cast<int>(address) << "...\n";
    std::cout << "Checking address: " << static_cast<int>(address) << std::endl;
    
    // Блок состояния input 0-27 (карта регистров v2) одной транзакцией
    std::vector<uint16_t> values;
    if (modbus->read_input_registers(address, constants::INPUT_STATUS_BLOCK_START,
                                     constants::INPUT_STATUS_BLOCK_COUNT, values) &&
        values.size() == constants::INPUT_STATUS_BLOCK_COUNT) {
        LOG_INFO("Successfully read input registers 0-27 from board address " + std::to_string(address) + ":");
        const uint16_t hv = values[constants::REG_HV_STATUS];
        const uint16_t gt = values[constants::REG_GAIN_TRIP_STATUS];
        std::cout << "  [0]  Update counter:      " << values[constants::REG_UPDATE_COUNTER] << "\n";
        std::cout << "  [1]  HV status:           0x" << std::hex << hv << std::dec
                  << "  (HV enable mask 0x" << std::hex << constants::hv_enabled_channels(hv)
                  << ", tripped analog boards 0x" << constants::tripped_analog_boards(hv) << std::dec << ")\n";
        std::cout << "  [2]  Gain/trip status:    0x" << std::hex << gt
                  << "  (trip enable 0x" << constants::trip_enable_bits(gt)
                  << ", gain 0x" << constants::gain_bits(gt) << std::dec << ")\n";
        for (size_t ch = 0; ch < constants::NUM_SIPM_CHANNELS; ++ch) {
            std::cout << "  ch " << ch
                      << ": current [" << constants::REG_SIPM_CURRENT_START + ch << "] = "
                      << values[constants::REG_SIPM_CURRENT_START + ch]
                      << ", temperature [" << constants::REG_SIPM_TEMP_START + ch << "] = "
                      << values[constants::REG_SIPM_TEMP_START + ch] << "\n";
        }
        std::cout << "  [27] CPU temperature:     " << values[constants::REG_CPU_TEMPERATURE] << "\n";
        return true;
    } else {
        LOG_ERROR("Failed to read from board: " + modbus->get_last_error());
        std::cout << "\nNo board found at address " << static_cast<int>(address) << ".\n";
        std::cout << "Consider using modbus_scanner to find active boards:\n";
        std::cout << "  ./modbus_scanner --show-addresses " << device << "\n\n";
        return false;
    }
}

int main(int argc, char* argv[]) {
    print_banner();
    
    // Parse command line arguments
    std::string experiment_name;
    std::string config_file = "config/board_map.json";
    bool standalone = false;
    bool test_modbus = false;
    bool verbose = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        }
        else if (arg == "-e" || arg == "--experiment") {
            if (i + 1 < argc) {
                experiment_name = argv[++i];
            }
        }
        else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[++i];
            }
        }
        else if (arg == "--standalone") {
            standalone = true;
        }
        else if (arg == "-t" || arg == "--test-modbus") {
            test_modbus = true;
        }
        else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        }
    }
    
    // Setup logging
    if (verbose) {
        Logger::instance().set_level(constants::LogLevel::DEBUG);
    }
    Logger::instance().set_log_file("danss_frontend.log");
    
    LOG_INFO("DANSS Slow Control Frontend starting");
    
    // Test mode
    if (test_modbus) {
        if (argc >= 4) {
            std::string device = argv[2];
            uint32_t baud_rate = std::stoul(argv[3]);
            uint8_t address = 1;  // Default address
            
            // Optional address parameter
            if (argc >= 5) {
                address = static_cast<uint8_t>(std::stoul(argv[4]));
            }
            
            bool success = test_modbus_communication(device, baud_rate, address);
            return success ? 0 : 1;
        } else {
            LOG_ERROR("Usage: danss_frontend --test-modbus <device> <baud_rate> [address]");
            return 1;
        }
    }
    
    // Standalone mode (without MIDAS)
    if (standalone) {
        LOG_INFO("Running in standalone mode (no MIDAS integration)");
        LOG_INFO("Configuration file: " + config_file);
        
        // TODO: Initialize RS485Manager, start polling
        
        LOG_INFO("Press Ctrl+C to stop");
        
        // Keep running
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        return 0;
    }
    
    // Normal MIDAS mode
    if (experiment_name.empty()) {
        LOG_ERROR("Experiment name required. Use -e <experiment_name>");
        print_help();
        return 1;
    }
    
    LOG_INFO("Starting MIDAS frontend for experiment: " + experiment_name);
    
    // TODO: Initialize MIDAS connection
    // TODO: Create equipment instances
    // TODO: Start main loop
    
    LOG_ERROR("MIDAS integration not yet implemented");
    LOG_INFO("Use --standalone mode for testing");
    
    return 1;
}
