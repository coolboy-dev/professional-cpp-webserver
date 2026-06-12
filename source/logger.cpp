#include "logger.h"
#include <iostream>
#include <iomanip>
#include <filesystem>

Logger& Logger::get_instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    flush_logs();
}

void Logger::init(const std::string& access_log_path, const std::string& error_log_path, LogLevel level) {
    std::lock_guard<std::mutex> access_lock(m_access_mutex);
    std::lock_guard<std::mutex> error_lock(m_error_mutex);

    m_log_level = level;
    m_console_output = false;
    m_is_initialized = true;

    try {
        ensure_log_directories();

        m_access_log_stream = std::make_unique<std::ofstream>(access_log_path, std::ios::app);
        m_error_log_stream = std::make_unique<std::ofstream>(error_log_path, std::ios::app);

        if (!m_access_log_stream->is_open()) {
            std::cerr << "Warning: Could not open access log file: " << access_log_path << std::endl;
            m_console_output = true;
        }

        if (!m_error_log_stream->is_open()) {
            std::cerr << "Warning: Could not open error log file: " << error_log_path << std::endl;
            m_console_output = true;
        }

        log_info("Logger initialized - Access: " + access_log_path + ", Error: " + error_log_path);

    } catch (const std::exception& e) {
        std::cerr << "Logger initialization error: " << e.what() << std::endl;
        m_console_output = true;
    }
}

void Logger::log_access(const std::string& client_ip,
                       const std::string& method,
                       const std::string& path,
                       int status_code,
                       size_t response_size,
                       const std::string& user_agent,
                       const std::string& referer) {

    if (!m_is_initialized) return;

    std::ostringstream log_entry;

    log_entry << client_ip << " - - [" << get_timestamp() << "] "
              << "\"" << method << " " << path << " HTTP/1.1\" "
              << status_code << " " << response_size;

    if (!referer.empty()) {
        log_entry << " \"" << referer << "\"";
    } else {
        log_entry << " \"-\"";
    }

    if (!user_agent.empty()) {
        log_entry << " \"" << user_agent << "\"";
    } else {
        log_entry << " \"-\"";
    }

    std::lock_guard<std::mutex> lock(m_access_mutex);
    if (m_access_log_stream && m_access_log_stream->is_open()) {
        *m_access_log_stream << log_entry.str() << std::endl;
        m_access_log_stream->flush();
    }

    if (m_console_output) {
        std::cout << "\033[32m[ACCESS]\033[0m " << log_entry.str() << std::endl;
    }
}

void Logger::log_error(const std::string& message, LogLevel level) {
    if (!m_is_initialized || level < m_log_level) return;

    std::ostringstream log_entry;
    log_entry << "[" << get_timestamp() << "] "
              << "[" << get_log_level_string(level) << "] "
              << message;

    std::lock_guard<std::mutex> lock(m_error_mutex);
    if (m_error_log_stream && m_error_log_stream->is_open()) {
        *m_error_log_stream << log_entry.str() << std::endl;
        m_error_log_stream->flush();
    }

    if (m_console_output || level >= LogLevel::ERROR) {
        std::string color = "\033[0m";
        if (level == LogLevel::ERROR) color = "\033[31m";
        else if (level == LogLevel::WARN) color = "\033[33m";
        else if (level == LogLevel::INFO) color = "\033[32m";
        else if (level == LogLevel::DEBUG) color = "\033[36m";

        std::cerr << color << log_entry.str() << "\033[0m" << std::endl;
    }
}

void Logger::log_info(const std::string& message) {
    log_error(message, LogLevel::INFO);
}

void Logger::log_warn(const std::string& message) {
    log_error(message, LogLevel::WARN);
}

void Logger::log_debug(const std::string& message) {
    log_error(message, LogLevel::DEBUG);
}

void Logger::flush_logs() {
    std::lock_guard<std::mutex> access_lock(m_access_mutex);
    std::lock_guard<std::mutex> error_lock(m_error_mutex);

    if (m_access_log_stream) {
        m_access_log_stream->flush();
    }
    if (m_error_log_stream) {
        m_error_log_stream->flush();
    }
}

void Logger::write_log(std::ofstream& file, const std::string& message) {
    if (file.is_open()) {
        file << message << std::endl;
        file.flush();
    }
}

std::string Logger::get_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%d/%b/%Y:%H:%M:%S %z");
    return oss.str();
}

std::string Logger::get_log_level_string(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default:              return "UNKNOWN";
    }
}

void Logger::ensure_log_directories() {
    try {
        std::filesystem::create_directories("benchmarks");
    } catch (const std::exception& e) {
        std::cerr << "Could not create benchmarks directory: " << e.what() << std::endl;
    }
}
