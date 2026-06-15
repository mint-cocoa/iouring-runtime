#pragma once

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace iouring::observability {

enum class LogLevel {
    kTrace,
    kDebug,
    kInfo,
    kWarn,
    kError,
    kCritical,
    kOff,
};

enum class LogCategory {
    kRuntime,
    kRing,
    kListener,
    kSession,
    kWeb,
    kHttp,
    kRouter,
    kProxy,
    kTls,
};

struct LoggingOptions {
    LogLevel level = LogLevel::kInfo;
    bool include_timestamp = true;
    bool include_thread_id = true;
};

std::optional<LogLevel> ParseLogLevel(std::string_view raw);
void ConfigureLogging(const LoggingOptions& options = {});
bool ConfigureLoggingFromEnv(std::string_view env_name = "IORUNTIME_LOG_LEVEL");
std::string_view CategoryName(LogCategory category);
bool ShouldLog(LogLevel level);
void LogMessage(LogLevel level, LogCategory category, std::string_view message);

template <typename... Args>
void Log(LogLevel level, LogCategory category,
         std::format_string<Args...> message,
         Args&&... args) {
    if (!ShouldLog(level)) {
        return;
    }
    LogMessage(level, category,
               std::format(message, std::forward<Args>(args)...));
}

template <typename... Args>
void LogTrace(LogCategory category, std::format_string<Args...> message,
              Args&&... args) {
    Log(LogLevel::kTrace, category, message, std::forward<Args>(args)...);
}

template <typename... Args>
void LogDebug(LogCategory category, std::format_string<Args...> message,
              Args&&... args) {
    Log(LogLevel::kDebug, category, message, std::forward<Args>(args)...);
}

template <typename... Args>
void LogInfo(LogCategory category, std::format_string<Args...> message,
             Args&&... args) {
    Log(LogLevel::kInfo, category, message, std::forward<Args>(args)...);
}

template <typename... Args>
void LogWarn(LogCategory category, std::format_string<Args...> message,
             Args&&... args) {
    Log(LogLevel::kWarn, category, message, std::forward<Args>(args)...);
}

template <typename... Args>
void LogError(LogCategory category, std::format_string<Args...> message,
              Args&&... args) {
    Log(LogLevel::kError, category, message, std::forward<Args>(args)...);
}

template <typename... Args>
void LogCritical(LogCategory category, std::format_string<Args...> message,
                 Args&&... args) {
    Log(LogLevel::kCritical, category, message, std::forward<Args>(args)...);
}

} // namespace iouring::observability
