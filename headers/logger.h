#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <memory>
#include <sstream>
#include <chrono>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static Logger& get_instance();

    void init(const std::string& access_log_path = "./logs/access.log",
              const std::string& error_log_path = "./logs/error.log",
              LogLevel level = LogLevel::INFO);

    void log_access(const std::string& client_ip,
                   const std::string& method,
                   const std::string& path,
                   int status_code,
                   size_t response_size,
                   const std::string& user_agent = "",
                   const std::string& referer = "");

    void log_error(const std::string& message, LogLevel level = LogLevel::ERROR);
    void log_info(const std::string& message);
    void log_warn(const std::string& message);
    void log_debug(const std::string& message);

    void set_log_level(LogLevel level) { m_log_level = level; }
    LogLevel get_log_level() const { return m_log_level; }

    void enable_console_output(bool enable) { m_console_output = enable; }
    void flush_logs();

private:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write_log(std::ofstream& file, const std::string& message);
    std::string get_timestamp() const;
    std::string get_log_level_string(LogLevel level) const;
    void ensure_log_directories();

    std::mutex m_access_mutex;
    std::mutex m_error_mutex;
    std::unique_ptr<std::ofstream> m_access_log_stream;
    std::unique_ptr<std::ofstream> m_error_log_stream;

    LogLevel m_log_level;
    bool m_console_output;
    bool m_is_initialized;
};
