#include "modbus/modbus_rtu.h"
#include "common/logger.h"
#include "common/constants.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

using namespace danss;

struct ScanResult {
    uint8_t address;
    bool responsive;
    uint16_t update_counter = 0;    // input 0
    uint16_t hv_status = 0;         // input 1: 0ttt eeee eeee eeee
    uint16_t gain_trip_status = 0;  // input 2: 0000 0rrr 0000 0aaa
    std::vector<uint16_t> status_block; // input 0-27
    std::string error_msg;
    double response_time_ms = 0.0;
};

// Список установленных битов: "-" если нет ни одного, иначе "0,2"
std::string bit_list(uint16_t bits, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        if ((bits >> i) & 1u) {
            if (!out.empty()) out += ",";
            out += std::to_string(i);
        }
    }
    return out.empty() ? "-" : out;
}

std::string hex4(uint16_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setw(4) << std::setfill('0') << v;
    return os.str();
}

void print_banner() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         DANSS Modbus Address Scanner                      ║\n";
    std::cout << "║         Scan RS-485 bus for active boards                 ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
}

void print_usage() {
    std::cout << "Usage: modbus_scanner [options] <device> [baud_rate]\n\n";
    std::cout << "Arguments:\n";
    std::cout << "  device         Serial device path (e.g., /dev/ttyUSB0)\n";
    std::cout << "  baud_rate      Communication speed (default: 19200)\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help           Show this help\n";
    std::cout << "  -s, --start <addr>   Start address (default: 1)\n";
    std::cout << "  -e, --end <addr>     End address (default: 247)\n";
    std::cout << "  -t, --timeout <ms>   Timeout per address (default: 500)\n";
    std::cout << "  -v, --verbose        Verbose output (show all attempts)\n";
    std::cout << "  -q, --quick          Summary table only (no per-channel details)\n";
    std::cout << "  --show-addresses     Show address being scanned (non-verbose)\n";
    std::cout << "  --delay <ms>         Delay between scans (default: 100)\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  modbus_scanner /dev/ttyUSB0\n";
    std::cout << "  modbus_scanner /dev/ttyUSB0 9600\n";
    std::cout << "  modbus_scanner -s 1 -e 30 /dev/ttyUSB0 19200\n";
    std::cout << "  modbus_scanner --quick /dev/ttyUSB0\n";
    std::cout << "  modbus_scanner --show-addresses /dev/ttyUSB0\n";
    std::cout << "\n";
}

ScanResult scan_address(ModbusRTU& modbus, uint8_t addr) {
    ScanResult result;
    result.address = addr;
    result.responsive = false;
    
    auto start = std::chrono::steady_clock::now();
    
    // Один запрос: блок состояния input 0-27 (карта v2).
    std::vector<uint16_t> block;
    bool success = modbus.read_input_registers(addr, constants::INPUT_STATUS_BLOCK_START,
                                               constants::INPUT_STATUS_BLOCK_COUNT, block);
    
    auto end = std::chrono::steady_clock::now();
    result.response_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (success && block.size() == constants::INPUT_STATUS_BLOCK_COUNT) {
        result.responsive = true;
        result.update_counter = block[constants::REG_UPDATE_COUNTER];
        result.hv_status = block[constants::REG_HV_STATUS];
        result.gain_trip_status = block[constants::REG_GAIN_TRIP_STATUS];
        result.status_block = std::move(block);
    } else {
        result.error_msg = success ? "Unexpected number of registers in response"
                                   : modbus.get_last_error();
    }
    
    return result;
}

void print_progress_bar(int current, int total, int width = 50) {
    float progress = static_cast<float>(current) / total;
    int pos = static_cast<int>(width * progress);
    
    std::cout << "\r[";
    for (int i = 0; i < width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << std::setw(3) << static_cast<int>(progress * 100) 
              << "% (" << current << "/" << total << ")";
    std::cout.flush();
}

void print_scan_summary(const std::vector<ScanResult>& results, bool verbose, bool quick_scan) {
    std::cout << "\n\n";
    std::cout << "╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    Scan Results                           ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";
    
    int found_count = 0;
    for (const auto& r : results) {
        if (r.responsive) found_count++;
    }
    
    std::cout << "Total addresses scanned: " << results.size() << "\n";
    std::cout << "Active boards found: " << found_count << "\n\n";
    
    if (found_count > 0) {
        std::cout << "Active boards:\n";
        std::cout << std::string(78, '-') << "\n";
        std::cout << std::setw(8) << "Address"
                  << std::setw(9) << "Update#"
                  << std::setw(9) << "HV st"
                  << std::setw(9) << "Trip"
                  << std::setw(10) << "TripEn"
                  << std::setw(9) << "Gain"
                  << std::setw(15) << "Response (ms)"
                  << "\n";
        std::cout << std::string(78, '-') << "\n";
        
        bool factory_found = false;
        for (const auto& r : results) {
            if (r.responsive) {
                std::cout << std::setw(8) << static_cast<int>(r.address)
                          << std::setw(9) << r.update_counter
                          << std::setw(9) << hex4(r.hv_status)
                          << std::setw(9) << bit_list(constants::tripped_analog_boards(r.hv_status), constants::NUM_ANALOG_BOARDS)
                          << std::setw(10) << bit_list(constants::trip_enable_bits(r.gain_trip_status), constants::NUM_ANALOG_BOARDS)
                          << std::setw(9) << bit_list(constants::gain_bits(r.gain_trip_status), constants::NUM_ANALOG_BOARDS)
                          << std::setw(15) << std::fixed << std::setprecision(2) << r.response_time_ms;
                if (r.address == constants::MODBUS_FACTORY_ADDRESS) {
                    std::cout << "  <- factory address (new board)";
                    factory_found = true;
                }
                std::cout << "\n";
            }
        }
        std::cout << std::string(78, '-') << "\n";
        std::cout << "Trip / TripEn / Gain: analog boards 0-" << constants::NUM_ANALOG_BOARDS - 1
                  << " with the bit set (\"-\" = none)\n";
        if (factory_found) {
            std::cout << "Board at address " << static_cast<int>(constants::MODBUS_FACTORY_ADDRESS)
                      << " has the factory address: assign a real address before use.\n";
        }
        
        // Подробности (кроме режима -q): HV по каналам, токи, температуры, CPU
        for (const auto& r : results) {
            if (quick_scan || !r.responsive || r.status_block.empty()) continue;
            const auto& b = r.status_block;
            std::cout << "\nBoard " << static_cast<int>(r.address) << ":\n";
            std::cout << "  HV enabled channels: "
                      << bit_list(constants::hv_enabled_channels(r.hv_status), constants::NUM_SIPM_CHANNELS) << "\n";
            std::cout << "  " << std::setw(4) << "ch" << std::setw(6) << "abrd"
                      << std::setw(10) << "current" << std::setw(8) << "temp" << std::setw(5) << "HV" << "\n";
            for (size_t ch = 0; ch < constants::NUM_SIPM_CHANNELS; ++ch) {
                std::cout << "  " << std::setw(4) << ch
                          << std::setw(6) << constants::analog_board_of_channel(ch)
                          << std::setw(10) << b[constants::REG_SIPM_CURRENT_START + ch]
                          << std::setw(8) << b[constants::REG_SIPM_TEMP_START + ch]
                          << std::setw(5) << (constants::hv_channel_enabled(r.hv_status, ch) ? "on" : "off")
                          << "\n";
            }
            std::cout << "  CPU temperature: " << b[constants::REG_CPU_TEMPERATURE] << " (raw)\n";
        }
        std::cout << "\n";
        
        // Generate suggested configuration
        std::cout << "\n";
        std::cout << "Suggested board_map.json configuration:\n";
        std::cout << "{\n";
        std::cout << "  \"boards\": [\n";
        
        bool first = true;
        for (const auto& r : results) {
            if (r.responsive && r.address != constants::MODBUS_FACTORY_ADDRESS) {
                if (!first) std::cout << ",\n";
                std::cout << "    {\n";
                std::cout << "      \"address\": " << static_cast<int>(r.address) << ",\n";
                std::cout << "      \"name\": \"PMT-XX-" << std::setw(2) << std::setfill('0') 
                          << static_cast<int>(r.address) << std::setfill(' ') << "\",\n";
                std::cout << "      \"enabled\": true\n";
                std::cout << "    }";
                first = false;
            }
        }
        std::cout << "\n  ]\n";
        std::cout << "}\n";
    }
    
    if (verbose && static_cast<size_t>(found_count) < results.size()) {
        std::cout << "\nNon-responsive addresses:\n";
        for (const auto& r : results) {
            if (!r.responsive) {
                std::cout << "  Address " << std::setw(3) << static_cast<int>(r.address) 
                          << ": " << r.error_msg << "\n";
            }
        }
    }
}

int main(int argc, char* argv[]) {
    print_banner();
    
    // Parse arguments
    std::string device;
    uint32_t baud_rate = 19200;
    uint8_t start_addr = 1;
    uint8_t end_addr = 247;
    uint32_t timeout_ms = 500;
    uint32_t delay_ms = 100;
    bool verbose = false;
    bool quick_scan = false;
    bool show_addresses = false;
    
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
        else if (arg == "-s" || arg == "--start") {
            if (i + 1 < argc) {
                start_addr = static_cast<uint8_t>(std::stoi(argv[++i]));
            }
        }
        else if (arg == "-e" || arg == "--end") {
            if (i + 1 < argc) {
                end_addr = static_cast<uint8_t>(std::stoi(argv[++i]));
            }
        }
        else if (arg == "-t" || arg == "--timeout") {
            if (i + 1 < argc) {
                timeout_ms = std::stoul(argv[++i]);
            }
        }
        else if (arg == "--delay") {
            if (i + 1 < argc) {
                delay_ms = std::stoul(argv[++i]);
            }
        }
        else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        }
        else if (arg == "-q" || arg == "--quick") {
            quick_scan = true;
        }
        else if (arg == "--show-addresses") {
            show_addresses = true;
        }
        else if (arg[0] != '-') {
            if (device.empty()) {
                device = arg;
            } else {
                baud_rate = std::stoul(arg);
            }
        }
        i++;
    }
    
    if (device.empty()) {
        std::cerr << "Error: Device path required\n\n";
        print_usage();
        return 1;
    }
    
    // Validate address range
    if (start_addr < constants::MODBUS_MIN_ADDRESS || 
        end_addr > constants::MODBUS_MAX_ADDRESS ||
        start_addr > end_addr) {
        std::cerr << "Error: Invalid address range. Must be 1-247.\n";
        return 1;
    }
    
    // Print scan parameters
    std::cout << "Scan parameters:\n";
    std::cout << "  Device: " << device << "\n";
    std::cout << "  Baud rate: " << baud_rate << "\n";
    std::cout << "  Address range: " << static_cast<int>(start_addr) 
              << " - " << static_cast<int>(end_addr) << "\n";
    std::cout << "  Timeout: " << timeout_ms << " ms\n";
    std::cout << "  Delay between scans: " << delay_ms << " ms\n";
    std::cout << "  Mode: " << (quick_scan ? "Summary only" : "Full report") << "\n";
    std::cout << "\n";
    
    // Setup logger
    danss::log::set_level(verbose ? constants::LogLevel::DEBUG : constants::LogLevel::INFO);
    
    // Open Modbus connection
    ModbusRTU modbus(device, baud_rate);
    modbus.set_timeout(timeout_ms);
    
    if (!modbus.open()) {
        std::cerr << "Error: Failed to open serial port: " << modbus.get_last_error() << "\n";
        return 1;
    }
    
    LOG_INFO("Serial port opened successfully");
    std::cout << "Starting scan...\n\n";
    
    // Perform scan
    std::vector<ScanResult> results;
    int total_addresses = end_addr - start_addr + 1;
    int current = 0;
    
    auto scan_start = std::chrono::steady_clock::now();
    
    for (uint8_t addr = start_addr; addr <= end_addr; addr++) {
        current++;
        
        if (verbose) {
            std::cout << "Scanning address " << std::setw(3) << static_cast<int>(addr) << "... ";
            std::cout.flush();
        } else if (show_addresses) {
            // Compact output showing current address on same line as progress
            std::cout << "\r[";
            int width = 50;
            float progress = static_cast<float>(current) / total_addresses;
            int pos = static_cast<int>(width * progress);
            
            for (int j = 0; j < width; ++j) {
                if (j < pos) std::cout << "=";
                else if (j == pos) std::cout << ">";
                else std::cout << " ";
            }
            std::cout << "] " << std::setw(3) << static_cast<int>(progress * 100) 
                      << "% (" << current << "/" << total_addresses << ") "
                      << "Address: " << std::setw(3) << static_cast<int>(addr) << "   ";
            std::cout.flush();
        } else {
            print_progress_bar(current, total_addresses);
        }
        
        ScanResult result = scan_address(modbus, addr);
        results.push_back(result);
        
        if (verbose) {
            if (result.responsive) {
                std::cout << "✓ FOUND (update #: " << result.update_counter 
                          << ", response: " << std::fixed << std::setprecision(1) 
                          << result.response_time_ms << " ms)\n";
            } else {
                std::cout << "✗ No response (" << result.error_msg << ")\n";
            }
        }
        
        // Small delay between scans to avoid overwhelming the bus
        if (addr < end_addr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    
    auto scan_end = std::chrono::steady_clock::now();
    double total_time = std::chrono::duration<double>(scan_end - scan_start).count();
    
    // Print summary
    print_scan_summary(results, verbose, quick_scan);
    
    std::cout << "\nScan completed in " << std::fixed << std::setprecision(2) 
              << total_time << " seconds\n";
    
    // Print statistics
    auto stats = modbus.get_statistics();
    std::cout << "\nCommunication statistics:\n";
    std::cout << "  Total requests: " << stats.total_requests << "\n";
    std::cout << "  Successful: " << stats.successful_responses << "\n";
    std::cout << "  Timeouts: " << stats.timeouts << "\n";
    std::cout << "  CRC errors: " << stats.crc_errors << "\n";
    std::cout << "  Average response time: " << std::fixed << std::setprecision(2) 
              << stats.avg_response_time_ms << " ms\n";
    
    return 0;
}
