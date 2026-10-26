#ifndef DANSS_LOGGER_H
#define DANSS_LOGGER_H

#include "common/constants.h"
#include <atomic>
#include <string>
#include <mutex>
#include <fstream>

namespace danss {

/// @brief Simple logging utility
/// TODO: Replace with proper logging library (e.g., spdlog)
class Logger {
public:
    static Logger& instance();
    
    void set_level(constants::LogLevel level);
    void set_log_file(const std::string& filename);
    
    void log(constants::LogLevel level, const std::string& message);
    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void critical(const std::string& message);
    
private:
    Logger() = default;
    std::mutex mutex_;
    std::atomic<constants::LogLevel> level_{constants::LogLevel::INFO};
    std::ofstream log_file_;
};

// Convenience macros
#define LOG_DEBUG(msg) danss::Logger::instance().debug(msg)
#define LOG_INFO(msg) danss::Logger::instance().info(msg)
#define LOG_WARNING(msg) danss::Logger::instance().warning(msg)
#define LOG_ERROR(msg) danss::Logger::instance().error(msg)
#define LOG_CRITICAL(msg) danss::Logger::instance().critical(msg)

namespace log {
inline void set_level(constants::LogLevel level) { Logger::instance().set_level(level); }
inline void set_file(const std::string& filename) { Logger::instance().set_log_file(filename); }
} // namespace log

} // namespace danss

#endif // DANSS_LOGGER_H
