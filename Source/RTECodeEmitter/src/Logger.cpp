#include "Logger.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace RTECodeEmitter {

namespace {

std::string CurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

}  // namespace

Logger::Logger(LogLevel level) : level_(level) {}

void Logger::Error(std::string_view message) const { Log(LogLevel::Error, message); }
void Logger::Warning(std::string_view message) const { Log(LogLevel::Warning, message); }
void Logger::Info(std::string_view message) const { Log(LogLevel::Info, message); }
void Logger::Debug(std::string_view message) const { Log(LogLevel::Debug, message); }
void Logger::Trace(std::string_view message) const { Log(LogLevel::Trace, message); }

bool Logger::IsEnabled(LogLevel level) const { return level <= level_; }

LogLevel Logger::ParseLevel(std::string_view level) {
    if (level == "error") return LogLevel::Error;
    if (level == "warning") return LogLevel::Warning;
    if (level == "info") return LogLevel::Info;
    if (level == "debug") return LogLevel::Debug;
    if (level == "trace") return LogLevel::Trace;
    return LogLevel::Info;
}

const char* Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Trace: return "TRACE";
    }
    return "UNKNOWN";
}

void Logger::Log(LogLevel level, std::string_view message) const {
    if (!IsEnabled(level)) return;

    std::ostringstream oss;
    oss << "[" << CurrentTimestamp() << "] [" << LevelToString(level) << "] " << message;

    if (level == LogLevel::Error || level == LogLevel::Warning) {
        std::cerr << oss.str() << '\n';
    } else {
        std::cout << oss.str() << '\n';
    }
}

}  // namespace RTECodeEmitter
