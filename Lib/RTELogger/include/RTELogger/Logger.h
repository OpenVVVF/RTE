#pragma once

#include <string>
#include <string_view>

namespace RTECodeEmitter {

enum class LogLevel {
    Error,
    Warning,
    Info,
    Debug,
    Trace,
};

class Logger {
public:
    explicit Logger(LogLevel level);

    void Error(std::string_view message) const;
    void Warning(std::string_view message) const;
    void Info(std::string_view message) const;
    void Debug(std::string_view message) const;
    void Trace(std::string_view message) const;

    bool IsEnabled(LogLevel level) const;

    static LogLevel ParseLevel(std::string_view level);
    static const char* LevelToString(LogLevel level);

private:
    void Log(LogLevel level, std::string_view message) const;

    LogLevel level_;
};

}  // namespace RTECodeEmitter
