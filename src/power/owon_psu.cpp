#include "power/owon_psu.h"
#include "common/logger.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifdef __linux__
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace danss {

using boost::asio::ip::tcp;

OwonPSU::OwonPSU(const std::string& host, uint16_t port)
    : socket_(nullptr)
    , host_(host)
    , port_(port)
    , timeout_ms_(1000)
    , selected_channel_(-1)
{
}

OwonPSU::~OwonPSU() {
    disconnect();
}

bool OwonPSU::connect() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (socket_ && socket_->is_open()) {
        return true;
    }

    try {
        socket_ = std::make_unique<tcp::socket>(io_context_);

        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host_, std::to_string(port_));

        // Bounded connect: async_connect + a deadline enforced via select()
        // on the raw fd, consistent with ModbusRTU's approach for RS-485 —
        // a hung TCP handshake (host unreachable, firewall dropping SYN)
        // must not block the poller thread indefinitely.
        boost::system::error_code ec = boost::asio::error::would_block;
        boost::asio::async_connect(*socket_, endpoints,
            [&ec](const boost::system::error_code& result_ec, const tcp::endpoint&) {
                ec = result_ec;
            });

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
        do {
            io_context_.run_one_for(std::chrono::milliseconds(50));
        } while (ec == boost::asio::error::would_block && std::chrono::steady_clock::now() < deadline);
        io_context_.restart();

        if (ec || !socket_->is_open()) {
            set_error("connect() failed/timed out: " + (ec ? ec.message() : std::string("timeout")));
            socket_.reset();
            return false;
        }

        socket_->set_option(tcp::no_delay(true));
        selected_channel_ = -1;
        LOG_INFO("OwonPSU: connected to " + host_ + ":" + std::to_string(port_));
        return true;

    } catch (const std::exception& e) {
        set_error(std::string("connect() exception: ") + e.what());
        socket_.reset();
        return false;
    }
}

void OwonPSU::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_) {
        boost::system::error_code ec;
        socket_->close(ec);
        socket_.reset();
    }
    selected_channel_ = -1;
}

bool OwonPSU::is_connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return socket_ && socket_->is_open();
}

void OwonPSU::set_timeout(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeout_ms_ = timeout_ms;
}

// =============================================================================
// Low-level line I/O
// =============================================================================

bool OwonPSU::write_line(const std::string& line) {
    if (!socket_ || !socket_->is_open()) {
        set_error("write_line(): not connected");
        return false;
    }

    std::string frame = line + "\n";
    boost::system::error_code ec;
    boost::asio::write(*socket_, boost::asio::buffer(frame), ec);
    if (ec) {
        set_error("write_line(): " + ec.message());
        return false;
    }
    return true;
}

bool OwonPSU::read_line(std::string& line_out) {
    if (!socket_ || !socket_->is_open()) {
        set_error("read_line(): not connected");
        return false;
    }

    line_out.clear();
    int fd = socket_->native_handle();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);

    char byte;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            set_error("read_line(): timeout waiting for response");
            return false;
        }
        long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;

        int ret = select(fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            set_error("read_line(): select() failed");
            return false;
        }
        if (ret == 0) {
            set_error("read_line(): timeout waiting for response");
            return false;
        }

        boost::system::error_code ec;
        size_t n = socket_->read_some(boost::asio::buffer(&byte, 1), ec);
        if (ec || n == 0) {
            set_error("read_line(): connection closed/error: " + ec.message());
            return false;
        }

        if (byte == '\n') {
            // Trim a trailing '\r' if the instrument sends CRLF
            if (!line_out.empty() && line_out.back() == '\r') {
                line_out.pop_back();
            }
            return true;
        }
        line_out.push_back(byte);
    }
}

bool OwonPSU::send_command(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto start = std::chrono::steady_clock::now();

    stats_.total_commands++;
    if (!write_line(cmd)) {
        stats_.protocol_errors++;
        return false;
    }

    double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    update_stats(true, elapsed_ms);
    return true;
}

bool OwonPSU::send_query(const std::string& cmd, std::string& reply_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto start = std::chrono::steady_clock::now();

    stats_.total_commands++;

    if (!write_line(cmd)) {
        stats_.protocol_errors++;
        return false;
    }

    if (!read_line(reply_out)) {
        // Distinguish timeout vs. hard error for statistics, mirroring ModbusRTU.
        if (last_error_.find("timeout") != std::string::npos) {
            stats_.timeouts++;
        } else {
            stats_.protocol_errors++;
        }
        return false;
    }

    double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    update_stats(true, elapsed_ms);
    return true;
}

// =============================================================================
// Identification / mode
// =============================================================================

bool OwonPSU::get_idn(std::string& idn_out) {
    return send_query("*IDN?", idn_out);
}

bool OwonPSU::set_remote(bool remote) {
    return send_command(remote ? "SYSTem:REMote" : "SYSTem:LOCal");
}

// =============================================================================
// Channel selection
// =============================================================================

bool OwonPSU::select_channel(uint8_t channel) {
    if (channel < 1 || channel > 3) {
        set_error("select_channel(): invalid channel " + std::to_string(channel));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (selected_channel_ == channel) {
            return true; // already selected — avoid a redundant round-trip
        }
    }
    std::ostringstream cmd;
    cmd << "INSTrument:NSELect " << static_cast<int>(channel);
    if (!send_command(cmd.str())) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    selected_channel_ = channel;
    return true;
}

// =============================================================================
// Configuration
// =============================================================================

namespace {
std::string format_value(double v) {
    // SCPI numeric arguments: plain decimal, no locale surprises.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}
} // namespace

bool OwonPSU::set_voltage(double volts) {
    return send_command("VOLTage " + format_value(volts));
}

bool OwonPSU::set_current(double amps) {
    return send_command("CURRent " + format_value(amps));
}

bool OwonPSU::set_voltage_protection(double volts) {
    return send_command("VOLTage:LIMit " + format_value(volts));
}

bool OwonPSU::set_current_protection(double amps) {
    return send_command("CURRent:LIMit " + format_value(amps));
}

bool OwonPSU::set_output(bool on) {
    return send_command(std::string("OUTPut:STATe ") + (on ? "1" : "0"));
}

// =============================================================================
// Measurements
// =============================================================================

bool OwonPSU::measure_voltage(double& volts_out) {
    std::string reply;
    if (!send_query("MEASure:VOLTage:DC?", reply)) {
        return false;
    }
    try {
        volts_out = std::stod(reply);
    } catch (const std::exception&) {
        set_error("measure_voltage(): unparsable reply '" + reply + "'");
        return false;
    }
    return true;
}

bool OwonPSU::measure_current(double& amps_out) {
    std::string reply;
    if (!send_query("MEASure:CURRent:DC?", reply)) {
        return false;
    }
    try {
        amps_out = std::stod(reply);
    } catch (const std::exception&) {
        set_error("measure_current(): unparsable reply '" + reply + "'");
        return false;
    }
    return true;
}

bool OwonPSU::measure_power(double& watts_out) {
    std::string reply;
    if (!send_query("MEASure:POWer:DC?", reply)) {
        return false;
    }
    try {
        watts_out = std::stod(reply);
    } catch (const std::exception&) {
        set_error("measure_power(): unparsable reply '" + reply + "'");
        return false;
    }
    return true;
}

namespace {
/// @brief Split a comma-separated SCPI reply ("12.000,12.000,5.500") into up to 3 doubles.
bool parse_csv3(const std::string& reply, std::array<double, 3>& out) {
    std::istringstream iss(reply);
    std::string token;
    size_t i = 0;
    while (std::getline(iss, token, ',') && i < out.size()) {
        try {
            out[i] = std::stod(token);
        } catch (const std::exception&) {
            return false;
        }
        i++;
    }
    return i == out.size();
}
} // namespace

bool OwonPSU::measure_all_voltages(std::array<double, 3>& out) {
    std::string reply;
    if (!send_query("MEASure:VOLTage:ALL:DC?", reply)) {
        return false;
    }
    if (!parse_csv3(reply, out)) {
        set_error("measure_all_voltages(): unparsable reply '" + reply + "'");
        return false;
    }
    return true;
}

bool OwonPSU::measure_all_currents(std::array<double, 3>& out) {
    std::string reply;
    if (!send_query("MEASure:CURRent:ALL:DC?", reply)) {
        return false;
    }
    if (!parse_csv3(reply, out)) {
        set_error("measure_all_currents(): unparsable reply '" + reply + "'");
        return false;
    }
    return true;
}

bool OwonPSU::get_all_output_states(std::array<bool, 3>& out) {
    std::string reply;
    if (!send_query("OUTPut:STATe:ALL?", reply)) {
        return false;
    }
    std::array<double, 3> raw{};
    if (!parse_csv3(reply, raw)) {
        set_error("get_all_output_states(): unparsable reply '" + reply + "'");
        return false;
    }
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = raw[i] != 0.0;
    }
    return true;
}

// =============================================================================
// Statistics / errors
// =============================================================================

OwonPSU::Statistics OwonPSU::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void OwonPSU::reset_statistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Statistics{};
}

void OwonPSU::set_error(const std::string& error) {
    last_error_ = error;
    LOG_ERROR("OwonPSU: " + error);
}

void OwonPSU::update_stats(bool success, double response_time_ms) {
    if (success) {
        stats_.successful_commands++;
        // Simple running average.
        uint64_t n = stats_.successful_commands;
        stats_.avg_response_time_ms += (response_time_ms - stats_.avg_response_time_ms) / static_cast<double>(n);
    }
}

} // namespace danss
