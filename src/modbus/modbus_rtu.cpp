#include "modbus/modbus_rtu.h"
#include "common/constants.h"
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

using namespace danss;
using namespace danss::constants;

namespace {

const char* exception_name(uint8_t code) {
    switch (code) {
        case MODBUS_EXCEPTION_ILLEGAL_FUNCTION:     return "illegal function";
        case MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS: return "illegal data address";
        case MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE:   return "illegal data value";
        case MODBUS_EXCEPTION_SLAVE_DEVICE_FAILURE: return "slave device failure";
        default:                                    return "unknown";
    }
}

std::string reply_mismatch(const std::vector<uint8_t>& request, const std::vector<uint8_t>& response) {
    switch (request[1]) {
        case MODBUS_FC_READ_HOLDING_REGISTERS:
        case MODBUS_FC_READ_INPUT_REGISTERS: {
            const size_t count = (static_cast<size_t>(request[4]) << 8) | request[5];
            if (response[2] != 2 * count) {
                return "Response has " + std::to_string(response[2] / 2) + " registers, expected "
                       + std::to_string(count);
            }
            return "";
        }
        case MODBUS_FC_WRITE_SINGLE_REGISTER:
            if (response != request) return "Write single register: response does not match request";
            return "";
        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
            if (!std::equal(request.begin(), request.begin() + 6, response.begin())) {
                return "Write multiple registers: response does not match request";
            }
            return "";
        default:
            return "";
    }
}

} // namespace

// =============================================================================
// Constructor / Destructor
// =============================================================================

ModbusRTU::ModbusRTU(const std::string& device, uint32_t baud_rate)
    : device_(device)
    , baud_rate_(baud_rate)
    , timeout_ms_(DEFAULT_MODBUS_TIMEOUT_MS)
{
}

ModbusRTU::~ModbusRTU() {
    close();
}

// =============================================================================
// Port Management
// =============================================================================

bool ModbusRTU::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        serial_port_ = std::make_unique<boost::asio::serial_port>(io_context_);
        serial_port_->open(device_);
        
        // Configure serial port for Modbus RTU
        serial_port_->set_option(boost::asio::serial_port::baud_rate(baud_rate_));
        serial_port_->set_option(boost::asio::serial_port::character_size(DEFAULT_DATA_BITS));
        serial_port_->set_option(boost::asio::serial_port::parity(boost::asio::serial_port::parity::none));
        serial_port_->set_option(boost::asio::serial_port::stop_bits(boost::asio::serial_port::stop_bits::one));
        serial_port_->set_option(boost::asio::serial_port::flow_control(boost::asio::serial_port::flow_control::none));
        
        return true;
        
    } catch (const boost::system::system_error& e) {
        set_error("Failed to open serial port: " + std::string(e.what()));
        serial_port_.reset();
        return false;
    }
}

void ModbusRTU::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (serial_port_ && serial_port_->is_open()) {
        boost::system::error_code ec;
        serial_port_->close(ec);
    }
    serial_port_.reset();
}

bool ModbusRTU::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_port_ && serial_port_->is_open();
}

void ModbusRTU::set_timeout(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeout_ms_ = timeout_ms;
}

// =============================================================================
// Modbus Functions
// =============================================================================

bool ModbusRTU::read_input_registers(
    uint8_t slave_addr,
    uint16_t start_reg,
    uint16_t count,
    std::vector<uint16_t>& values)
{
    auto request = build_read_request(slave_addr, MODBUS_FC_READ_INPUT_REGISTERS, start_reg, count);
    std::vector<uint8_t> response;
    
    if (!transact(request, response)) {
        return false;
    }
    
    parse_read_response(response, values);
    return true;
}

bool ModbusRTU::read_holding_registers(
    uint8_t slave_addr,
    uint16_t start_reg,
    uint16_t count,
    std::vector<uint16_t>& values)
{
    auto request = build_read_request(slave_addr, MODBUS_FC_READ_HOLDING_REGISTERS, start_reg, count);
    std::vector<uint8_t> response;
    
    if (!transact(request, response)) {
        return false;
    }
    
    parse_read_response(response, values);
    return true;
}

bool ModbusRTU::write_single_register(
    uint8_t slave_addr,
    uint16_t reg_addr,
    uint16_t value)
{
    auto request = build_write_single_request(slave_addr, reg_addr, value);
    std::vector<uint8_t> response;
    
    return transact(request, response);
}

bool ModbusRTU::write_multiple_registers(
    uint8_t slave_addr,
    uint16_t start_reg,
    const std::vector<uint16_t>& values)
{
    assert(!values.empty() && values.size() <= MODBUS_MAX_WRITE_REGISTERS && "write: count must be 1-123");
    auto request = build_write_multiple_request(slave_addr, start_reg, values);
    std::vector<uint8_t> response;
    
    return transact(request, response);
}

// =============================================================================
// Internal Transaction Method
// =============================================================================

bool ModbusRTU::transact(const std::vector<uint8_t>& request, std::vector<uint8_t>& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!serial_port_ || !serial_port_->is_open()) {
        set_error("Serial port not open");
        return false;
    }
    
    try {
        boost::system::error_code ec;
        
        // Wait T3.5 inter-frame delay before sending
        std::this_thread::sleep_for(std::chrono::microseconds(MODBUS_T35_CHAR_US));
        tcflush(serial_port_->native_handle(), TCIFLUSH);
        auto start_time = std::chrono::steady_clock::now();
        
        // Send request
        boost::asio::write(*serial_port_, boost::asio::buffer(request), ec);
        if (ec) {
            set_error("Write error: " + ec.message());
            update_stats(false, 0.0);
            return false;
        }
        
        // Read response
        response.clear();
        response.resize(MODBUS_RTU_MAX_FRAME_SIZE);

        auto frame_length = [&response](size_t have) -> size_t {
            if (have < 2) return 0;
            const uint8_t fc = response[1];
            if (fc & 0x80) return 5;
            if (fc == MODBUS_FC_WRITE_SINGLE_REGISTER || fc == MODBUS_FC_WRITE_MULTIPLE_REGISTERS) return 8;
            if (fc == MODBUS_FC_READ_HOLDING_REGISTERS || fc == MODBUS_FC_READ_INPUT_REGISTERS)
                return have < 3 ? 0 : 5 + static_cast<size_t>(response[2]);
            return have;
        };

        size_t bytes_read = 0;
        size_t expected = 0;
        const int fd = serial_port_->native_handle();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);

        while (expected == 0 || bytes_read < expected) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) {
                break;
            }

            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);
            struct timeval tv;
            tv.tv_sec = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;
            int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                set_error(std::string("Select error: ") + std::strerror(errno));
                update_stats(false, 0.0);
                return false;
            }
            if (ret == 0) {
                break;
            }

            size_t to_read = expected ? expected - bytes_read : (bytes_read < 2 ? 2 : 3) - bytes_read;
            size_t n = serial_port_->read_some(
                boost::asio::buffer(response.data() + bytes_read, to_read), ec);
            if (ec) {
                set_error("Read error: " + ec.message());
                update_stats(false, 0.0);
                return false;
            }
            bytes_read += n;
            expected = frame_length(bytes_read);

            if (expected > MODBUS_RTU_MAX_FRAME_SIZE) {
                set_error("Invalid byte count in response");
                stats_.protocol_errors++;
                update_stats(false, 0.0);
                return false;
            }
        }

        if (bytes_read > 0 && (expected == 0 || bytes_read < expected)) {
            set_error("Incomplete response: got " + std::to_string(bytes_read) + " of "
                      + (expected ? std::to_string(expected) : std::string("?")) + " bytes");
            stats_.timeouts++;
            update_stats(false, 0.0);
            return false;
        }

        if (bytes_read == 0) {
            set_error("Timeout waiting for response");
            stats_.timeouts++;
            update_stats(false, 0.0);
            return false;
        }

        if (bytes_read < MODBUS_RTU_MIN_FRAME_SIZE) {
            set_error("Response too short: " + std::to_string(bytes_read)
                      + " bytes, function code " + std::to_string(response[1]));
            stats_.protocol_errors++;
            update_stats(false, 0.0);
            return false;
        }

        response.resize(bytes_read);
        
        // Verify CRC
        if (!verify_crc(response)) {
            set_error("CRC error in response");
            stats_.crc_errors++;
            update_stats(false, 0.0);
            return false;
        }
        if (response[0] != request[0]) {
            set_error("Unexpected slave address in response: " + std::to_string(response[0])
                      + " instead of " + std::to_string(request[0]));
            stats_.protocol_errors++;
            update_stats(false, 0.0);
            return false;
        }
        if ((response[1] & 0x7F) != request[1]) {
            set_error("Unexpected function code in response: " + std::to_string(response[1]));
            stats_.protocol_errors++;
            update_stats(false, 0.0);
            return false;
        }
        if (response[1] & 0x80) {
            set_error("Modbus exception " + std::to_string(response[2]) + " ("
                      + exception_name(response[2]) + ")");
            stats_.exception_responses++;
            update_stats(false, 0.0);
            return false;
        }
        const std::string mismatch = reply_mismatch(request, response);
        if (!mismatch.empty()) {
            set_error(mismatch);
            stats_.protocol_errors++;
            update_stats(false, 0.0);
            return false;
        }
        
        // Calculate response time
        auto end_time = std::chrono::steady_clock::now();
        double response_time = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
        update_stats(true, response_time);
        return true;
        
    } catch (const boost::system::system_error& e) {
        set_error("Transaction error: " + std::string(e.what()));
        update_stats(false, 0.0);
        return false;
    }
}

// =============================================================================
// Request Builders
// =============================================================================

std::vector<uint8_t> ModbusRTU::build_read_request(
    uint8_t slave_addr,
    uint8_t function_code,
    uint16_t start_reg,
    uint16_t count)
{
    std::vector<uint8_t> request;
    request.reserve(8);
    
    request.push_back(slave_addr);
    request.push_back(function_code);
    request.push_back(static_cast<uint8_t>(start_reg >> 8));   // Start address high
    request.push_back(static_cast<uint8_t>(start_reg & 0xFF)); // Start address low
    request.push_back(static_cast<uint8_t>(count >> 8));       // Count high
    request.push_back(static_cast<uint8_t>(count & 0xFF));     // Count low
    
    append_crc(request);
    return request;
}

std::vector<uint8_t> ModbusRTU::build_write_single_request(
    uint8_t slave_addr,
    uint16_t reg_addr,
    uint16_t value)
{
    std::vector<uint8_t> request;
    request.reserve(8);
    
    request.push_back(slave_addr);
    request.push_back(MODBUS_FC_WRITE_SINGLE_REGISTER);
    request.push_back(static_cast<uint8_t>(reg_addr >> 8));
    request.push_back(static_cast<uint8_t>(reg_addr & 0xFF));
    request.push_back(static_cast<uint8_t>(value >> 8));
    request.push_back(static_cast<uint8_t>(value & 0xFF));
    
    append_crc(request);
    return request;
}

std::vector<uint8_t> ModbusRTU::build_write_multiple_request(
    uint8_t slave_addr,
    uint16_t start_reg,
    const std::vector<uint16_t>& values)
{
    std::vector<uint8_t> request;
    uint16_t count = static_cast<uint16_t>(values.size());
    uint8_t byte_count = static_cast<uint8_t>(count * 2);
    
    request.reserve(9 + byte_count);
    
    request.push_back(slave_addr);
    request.push_back(MODBUS_FC_WRITE_MULTIPLE_REGISTERS);
    request.push_back(static_cast<uint8_t>(start_reg >> 8));
    request.push_back(static_cast<uint8_t>(start_reg & 0xFF));
    request.push_back(static_cast<uint8_t>(count >> 8));
    request.push_back(static_cast<uint8_t>(count & 0xFF));
    request.push_back(byte_count);
    
    for (uint16_t value : values) {
        request.push_back(static_cast<uint8_t>(value >> 8));
        request.push_back(static_cast<uint8_t>(value & 0xFF));
    }
    
    append_crc(request);
    return request;
}

// =============================================================================
// Response Parser
// =============================================================================

void ModbusRTU::parse_read_response(
    const std::vector<uint8_t>& response,
    std::vector<uint16_t>& values)
{
    const size_t byte_count = response[2];
    values.clear();
    values.reserve(byte_count / 2);
    
    for (size_t i = 0; i < byte_count; i += 2) {
        uint16_t value = (static_cast<uint16_t>(response[3 + i]) << 8) |
                         static_cast<uint16_t>(response[3 + i + 1]);
        values.push_back(value);
    }
}

// =============================================================================
// Statistics
// =============================================================================

ModbusRTU::Statistics ModbusRTU::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void ModbusRTU::reset_statistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Statistics{};
}

// =============================================================================
// Helper Methods
// =============================================================================

std::string ModbusRTU::get_last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

uint32_t ModbusRTU::get_timeout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timeout_ms_;
}

void ModbusRTU::set_error(const std::string& error) {
    last_error_ = error;
}

void ModbusRTU::update_stats(bool success, double response_time_ms) {
    stats_.total_requests++;
    
    if (success) {
        stats_.successful_responses++;
        
        // Update moving average
        double n = static_cast<double>(stats_.successful_responses);
        stats_.avg_response_time_ms = 
            (stats_.avg_response_time_ms * (n - 1.0) + response_time_ms) / n;
    }
}

// CRC calculation is in separate file (modbus_crc.cpp)
