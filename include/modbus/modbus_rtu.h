#ifndef DANSS_MODBUS_RTU_H
#define DANSS_MODBUS_RTU_H

#include <boost/asio.hpp>
#include <boost/asio/serial_port.hpp>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <mutex>

namespace danss {

/// @brief Modbus RTU communication class using Boost.Asio
/// 
/// Implements Modbus RTU protocol over RS-485 serial connection.
/// Thread-safe for concurrent access.
class ModbusRTU {
public:
    /// @brief Constructor
    /// @param device Serial device path (e.g., /dev/ttyUSB0)
    /// @param baud_rate Communication speed (default: 19200)
    explicit ModbusRTU(const std::string& device, uint32_t baud_rate = 19200);
    
    /// @brief Destructor - closes serial port
    ~ModbusRTU();
    
    // Disable copy and move
    ModbusRTU(const ModbusRTU&) = delete;
    ModbusRTU& operator=(const ModbusRTU&) = delete;
    ModbusRTU(ModbusRTU&&) = delete;
    ModbusRTU& operator=(ModbusRTU&&) = delete;
    
    /// @brief Open serial port and configure
    /// @return true on success
    bool open();
    
    /// @brief Close serial port
    void close();
    
    /// @brief Check if port is open
    bool is_open() const;
    
    /// @brief Set response timeout
    /// @param timeout_ms Timeout in milliseconds
    void set_timeout(uint32_t timeout_ms);
    
    /// @brief Get current timeout
    uint32_t get_timeout() const { return timeout_ms_; }
    
    // =========================================================================
    // Modbus Functions
    // =========================================================================
    
    /// @brief Read Input Registers (Function Code 0x04)
    /// @param slave_addr Modbus slave address (1-247)
    /// @param start_reg Starting register address
    /// @param count Number of registers to read
    /// @param[out] values Output vector for register values
    /// @return true on success
    bool read_input_registers(
        uint8_t slave_addr,
        uint16_t start_reg,
        uint16_t count,
        std::vector<uint16_t>& values
    );
    
    /// @brief Read Holding Registers (Function Code 0x03)
    /// @param slave_addr Modbus slave address (1-247)
    /// @param start_reg Starting register address
    /// @param count Number of registers to read
    /// @param[out] values Output vector for register values
    /// @return true on success
    bool read_holding_registers(
        uint8_t slave_addr,
        uint16_t start_reg,
        uint16_t count,
        std::vector<uint16_t>& values
    );
    
    /// @brief Write Single Register (Function Code 0x06)
    /// @param slave_addr Modbus slave address (1-247)
    /// @param reg_addr Register address
    /// @param value Value to write
    /// @return true on success
    bool write_single_register(
        uint8_t slave_addr,
        uint16_t reg_addr,
        uint16_t value
    );
    
    /// @brief Write Multiple Registers (Function Code 0x10)
    /// @param slave_addr Modbus slave address (1-247)
    /// @param start_reg Starting register address
    /// @param values Vector of values to write
    /// @return true on success
    bool write_multiple_registers(
        uint8_t slave_addr,
        uint16_t start_reg,
        const std::vector<uint16_t>& values
    );
    
    // =========================================================================
    // Statistics
    // =========================================================================
    
    struct Statistics {
        uint64_t total_requests = 0;
        uint64_t successful_responses = 0;
        uint64_t timeouts = 0;
        uint64_t crc_errors = 0;
        uint64_t exception_responses = 0;
        double avg_response_time_ms = 0.0;
    };
    
    /// @brief Get communication statistics
    Statistics get_statistics() const;
    
    /// @brief Reset statistics counters
    void reset_statistics();
    
    /// @brief Get last error message
    std::string get_last_error() const { return last_error_; }

private:
    // =========================================================================
    // Internal methods
    // =========================================================================
    
    /// @brief Send Modbus request and receive response
    /// @param request Request frame
    /// @param[out] response Response frame
    /// @return true on success
    bool transact(const std::vector<uint8_t>& request, std::vector<uint8_t>& response);
    
    /// @brief Build read registers request
    std::vector<uint8_t> build_read_request(
        uint8_t slave_addr,
        uint8_t function_code,
        uint16_t start_reg,
        uint16_t count
    );
    
    /// @brief Parse read registers response
    bool parse_read_response(
        const std::vector<uint8_t>& response,
        uint8_t expected_function,
        std::vector<uint16_t>& values
    );
    
    /// @brief Build write single register request
    std::vector<uint8_t> build_write_single_request(
        uint8_t slave_addr,
        uint16_t reg_addr,
        uint16_t value
    );
    
    /// @brief Build write multiple registers request
    std::vector<uint8_t> build_write_multiple_request(
        uint8_t slave_addr,
        uint16_t start_reg,
        const std::vector<uint16_t>& values
    );
    
    /// @brief Validate response and check for exceptions
    bool validate_response(
        const std::vector<uint8_t>& response,
        uint8_t expected_slave,
        uint8_t expected_function
    );
    
    /// @brief Calculate Modbus RTU CRC16
    static uint16_t calculate_crc(const std::vector<uint8_t>& data);
    
    /// @brief Append CRC to frame
    static void append_crc(std::vector<uint8_t>& frame);
    
    /// @brief Verify CRC in received frame
    static bool verify_crc(const std::vector<uint8_t>& frame);
    
    /// @brief Set last error message
    void set_error(const std::string& error);
    
    /// @brief Update statistics
    void update_stats(bool success, double response_time_ms);
    
    // =========================================================================
    // Member variables
    // =========================================================================
    
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::serial_port> serial_port_;
    
    std::string device_;
    uint32_t baud_rate_;
    uint32_t timeout_ms_;
    
    mutable std::mutex mutex_;  // Thread safety
    std::string last_error_;
    
    Statistics stats_;
};

} // namespace danss

#endif // DANSS_MODBUS_RTU_H
