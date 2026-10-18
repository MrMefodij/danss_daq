#include "modbus/modbus_rtu.h"
#include "common/constants.h"
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <thread>

#ifdef __linux__
#include <sys/select.h>
#include <unistd.h>
#endif

using namespace danss;
using namespace danss::constants;

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
    
    return parse_read_response(response, MODBUS_FC_READ_INPUT_REGISTERS, values);
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
    
    return parse_read_response(response, MODBUS_FC_READ_HOLDING_REGISTERS, values);
}

bool ModbusRTU::write_single_register(
    uint8_t slave_addr,
    uint16_t reg_addr,
    uint16_t value)
{
    auto request = build_write_single_request(slave_addr, reg_addr, value);
    std::vector<uint8_t> response;
    
    if (!transact(request, response)) {
        return false;
    }
    
    // Validate echo response
    if (response.size() != request.size()) {
        set_error("Write single register: Invalid response size");
        return false;
    }
    
    // Response should echo the request (minus CRC which we already verified)
    for (size_t i = 0; i < response.size() - 2; ++i) {
        if (response[i] != request[i]) {
            set_error("Write single register: Response does not match request");
            return false;
        }
    }
    
    return true;
}

bool ModbusRTU::write_multiple_registers(
    uint8_t slave_addr,
    uint16_t start_reg,
    const std::vector<uint16_t>& values)
{
    auto request = build_write_multiple_request(slave_addr, start_reg, values);
    std::vector<uint8_t> response;
    
    if (!transact(request, response)) {
        return false;
    }
    
    return validate_response(response, slave_addr, MODBUS_FC_WRITE_MULTIPLE_REGISTERS);
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
    
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        boost::system::error_code ec;
        
        // Wait T3.5 inter-frame delay before sending
        std::this_thread::sleep_for(std::chrono::microseconds(MODBUS_T35_CHAR_US));
        
        // Send request
        boost::asio::write(*serial_port_, boost::asio::buffer(request), ec);
        if (ec) {
            set_error("Write error: " + ec.message());
            return false;
        }
        
        // Read response with timeout using sync operations with deadline
        response.clear();
        response.resize(MODBUS_RTU_MAX_FRAME_SIZE);
        
        size_t bytes_read = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
        
        // Set a short timeout for initial byte
        bool got_first_byte = false;
        
        while (std::chrono::steady_clock::now() < deadline) {
            // Try to read one byte at a time with very short timeout
            ec.clear();
            
            // Use read with timeout by checking time before blocking read
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            
            if (remaining <= 0) {
                break; // Timeout
            }
            
            // Non-blocking read attempt
            #ifdef __linux__
            // On Linux, we can use native_handle to set non-blocking mode temporarily
            int fd = serial_port_->native_handle();
            
            // Use select() to wait for data with timeout
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);
            
            struct timeval tv;
            tv.tv_sec = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;
            
            int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
            
            if (ret > 0) {
                // Data available, read it
                size_t n = boost::asio::read(*serial_port_,
                    boost::asio::buffer(response.data() + bytes_read, 1), ec);
                
                if (ec && ec != boost::asio::error::eof) {
                    set_error("Read error: " + ec.message());
                    return false;
                }
                
                if (n > 0) {
                    bytes_read += n;
                    got_first_byte = true;
                    
                    // If we got the first byte, read the rest more aggressively
                    if (bytes_read >= MODBUS_RTU_MIN_FRAME_SIZE) {
                        // Wait for frame to complete (T3.5)
                        std::this_thread::sleep_for(std::chrono::microseconds(MODBUS_T35_CHAR_US * 2));
                        
                        // Try to read any remaining bytes
                        FD_ZERO(&read_fds);
                        FD_SET(fd, &read_fds);
                        tv.tv_sec = 0;
                        tv.tv_usec = 1000; // 1ms
                        
                        ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
                        if (ret > 0) {
                            // More data available
                            size_t remaining_space = response.size() - bytes_read;
                            n = boost::asio::read(*serial_port_,
                                boost::asio::buffer(response.data() + bytes_read, remaining_space), 
                                boost::asio::transfer_at_least(1), ec);
                            bytes_read += n;
                        }
                        
                        // Frame should be complete now
                        break;
                    }
                }
            } else if (ret == 0) {
                // Timeout on this iteration
                if (got_first_byte && bytes_read >= MODBUS_RTU_MIN_FRAME_SIZE) {
                    // We have minimum frame, consider it complete
                    break;
                }
                // Otherwise continue waiting
            } else {
                // Error in select
                set_error("Select error");
                return false;
            }
            #else
            // Fallback for non-Linux: simple blocking read with overall timeout
            size_t n = boost::asio::read(*serial_port_,
                boost::asio::buffer(response.data() + bytes_read, 1), ec);
            
            if (ec && ec != boost::asio::error::eof) {
                set_error("Read error: " + ec.message());
                return false;
            }
            
            if (n > 0) {
                bytes_read += n;
                if (bytes_read >= MODBUS_RTU_MIN_FRAME_SIZE) {
                    std::this_thread::sleep_for(std::chrono::microseconds(MODBUS_T35_CHAR_US * 2));
                    break;
                }
            }
            #endif
        }
        
        if (bytes_read == 0) {
            set_error("Timeout waiting for response");
            stats_.timeouts++;
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

bool ModbusRTU::parse_read_response(
    const std::vector<uint8_t>& response,
    uint8_t expected_function,
    std::vector<uint16_t>& values)
{
    if (response.size() < 5) { // Min: addr + fc + byte_count + crc(2)
        set_error("Response too short");
        return false;
    }
    
    uint8_t function_code = response[1];
    
    // Check for exception
    if (function_code & 0x80) {
        uint8_t exception_code = response[2];
        set_error("Modbus exception: " + std::to_string(exception_code));
        stats_.exception_responses++;
        return false;
    }
    
    if (function_code != expected_function) {
        set_error("Unexpected function code in response");
        return false;
    }
    
    uint8_t byte_count = response[2];
    size_t expected_size = 3 + byte_count + 2; // addr + fc + count + data + crc
    
    if (response.size() != expected_size) {
        set_error("Response size mismatch");
        return false;
    }
    
    // Extract register values
    values.clear();
    values.reserve(byte_count / 2);
    
    for (size_t i = 0; i < byte_count; i += 2) {
        uint16_t value = (static_cast<uint16_t>(response[3 + i]) << 8) |
                         static_cast<uint16_t>(response[3 + i + 1]);
        values.push_back(value);
    }
    
    return true;
}

bool ModbusRTU::validate_response(
    const std::vector<uint8_t>& response,
    uint8_t expected_slave,
    uint8_t expected_function)
{
    if (response.size() < MODBUS_RTU_MIN_FRAME_SIZE) {
        set_error("Response too short");
        return false;
    }
    
    if (response[0] != expected_slave) {
        set_error("Unexpected slave address in response");
        return false;
    }
    
    uint8_t function_code = response[1];
    
    if (function_code & 0x80) {
        uint8_t exception_code = response[2];
        set_error("Modbus exception: " + std::to_string(exception_code));
        stats_.exception_responses++;
        return false;
    }
    
    if (function_code != expected_function) {
        set_error("Unexpected function code in response");
        return false;
    }
    
    return true;
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
