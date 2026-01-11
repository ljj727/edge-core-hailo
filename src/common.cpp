#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace stream_daemon {

namespace {

[[nodiscard]] std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

[[nodiscard]] constexpr std::string_view LogLevelToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kDebug:   return "DEBUG";
        case LogLevel::kInfo:    return "INFO ";
        case LogLevel::kWarning: return "WARN ";
        case LogLevel::kError:   return "ERROR";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view LogLevelColor(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kDebug:   return "\033[36m";  // Cyan
        case LogLevel::kInfo:    return "\033[32m";  // Green
        case LogLevel::kWarning: return "\033[33m";  // Yellow
        case LogLevel::kError:   return "\033[31m";  // Red
    }
    return "\033[0m";
}

constexpr std::string_view kColorReset = "\033[0m";

}  // namespace

void Log(LogLevel level, std::string_view message) {
    std::ostringstream oss;
    oss << LogLevelColor(level)
        << "[" << GetTimestamp() << "] "
        << "[" << LogLevelToString(level) << "] "
        << kColorReset
        << message;

    if (level == LogLevel::kError) {
        std::cerr << oss.str() << std::endl;
    } else {
        std::cout << oss.str() << std::endl;
    }
}

void LogDebug(std::string_view message) {
#ifdef DEBUG
    Log(LogLevel::kDebug, message);
#else
    (void)message;
#endif
}

void LogInfo(std::string_view message) {
    Log(LogLevel::kInfo, message);
}

void LogWarning(std::string_view message) {
    Log(LogLevel::kWarning, message);
}

void LogError(std::string_view message) {
    Log(LogLevel::kError, message);
}

}  // namespace stream_daemon
