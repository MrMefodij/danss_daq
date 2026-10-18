#include "common/logger.h"
#include <iostream>
#include <iomanip>
#include <chrono>

using namespace danss;

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::set_level(constants::LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

void Logger::set_log_file(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
        log_file_.close();
    }
    log_file_.open(filename, std::ios::app);
}

void Logger::log(constants::LogLevel level, const std::string& message) {
    if (level < level_) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    // Format: [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
    
    const char* level_str = "UNKNOWN";
    switch (level) {
        case constants::LogLevel::DEBUG:    level_str = "DEBUG";    break;
        case constants::LogLevel::INFO:     level_str = "INFO";     break;
        case constants::LogLevel::WARNING:  level_str = "WARNING";  break;
        case constants::LogLevel::ERROR:    level_str = "ERROR";    break;
        case constants::LogLevel::CRITICAL: level_str = "CRITICAL"; break;
    }
    
    std::string formatted = std::string("[") + time_buf + "." + 
                           std::to_string(ms.count()) + "] [" + 
                           level_str + "] " + message;
    
    std::cout << formatted << std::endl;
    
    if (log_file_.is_open()) {
        log_file_ << formatted << std::endl;
        log_file_.flush();
    }
}

void Logger::debug(const std::string& message) { log(constants::LogLevel::DEBUG, message); }
void Logger::info(const std::string& message) { log(constants::LogLevel::INFO, message); }
void Logger::warning(const std::string& message) { log(constants::LogLevel::WARNING, message); }
void Logger::error(const std::string& message) { log(constants::LogLevel::ERROR, message); }
void Logger::critical(const std::string& message) { log(constants::LogLevel::CRITICAL, message); }
