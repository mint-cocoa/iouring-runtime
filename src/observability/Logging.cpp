#include <iouring/observability/Logging.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace iouring::observability {
namespace {

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
    case LogLevel::kTrace:
        return spdlog::level::trace;
    case LogLevel::kDebug:
        return spdlog::level::debug;
    case LogLevel::kInfo:
        return spdlog::level::info;
    case LogLevel::kWarn:
        return spdlog::level::warn;
    case LogLevel::kError:
        return spdlog::level::err;
    case LogLevel::kCritical:
        return spdlog::level::critical;
    case LogLevel::kOff:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

std::string Lowercase(std::string_view raw) {
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

std::optional<LogLevel> ParseLogLevel(std::string_view raw) {
    const std::string value = Lowercase(raw);
    if (value == "trace") {
        return LogLevel::kTrace;
    }
    if (value == "debug") {
        return LogLevel::kDebug;
    }
    if (value == "info") {
        return LogLevel::kInfo;
    }
    if (value == "warn" || value == "warning") {
        return LogLevel::kWarn;
    }
    if (value == "error" || value == "err") {
        return LogLevel::kError;
    }
    if (value == "critical" || value == "fatal") {
        return LogLevel::kCritical;
    }
    if (value == "off" || value == "none") {
        return LogLevel::kOff;
    }
    return std::nullopt;
}

void ConfigureLogging(const LoggingOptions& options) {
    spdlog::set_level(ToSpdlogLevel(options.level));

    if (options.include_timestamp && options.include_thread_id) {
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");
    } else if (options.include_timestamp) {
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    } else if (options.include_thread_id) {
        spdlog::set_pattern("[%t] [%^%l%$] %v");
    } else {
        spdlog::set_pattern("[%^%l%$] %v");
    }
}

bool ConfigureLoggingFromEnv(std::string_view env_name) {
    const std::string name(env_name);
    const char* raw = std::getenv(name.c_str());
    if (raw == nullptr) {
        return false;
    }

    auto level = ParseLogLevel(raw);
    if (!level.has_value()) {
        return false;
    }

    ConfigureLogging({.level = *level});
    return true;
}

bool ShouldLog(LogLevel level) {
    if (level == LogLevel::kOff) {
        return false;
    }
    auto* logger = spdlog::default_logger_raw();
    return logger != nullptr && logger->should_log(ToSpdlogLevel(level));
}

void LogMessage(LogLevel level, LogCategory category, std::string_view message) {
    spdlog::log(ToSpdlogLevel(level), "[{}] {}", CategoryName(category), message);
}

std::string_view CategoryName(LogCategory category) {
    switch (category) {
    case LogCategory::kRuntime:
        return "runtime";
    case LogCategory::kRing:
        return "ring";
    case LogCategory::kListener:
        return "listener";
    case LogCategory::kSession:
        return "session";
    case LogCategory::kWeb:
        return "web";
    case LogCategory::kHttp:
        return "http";
    case LogCategory::kRouter:
        return "router";
    case LogCategory::kProxy:
        return "proxy";
    case LogCategory::kTls:
        return "tls";
    }
    return "runtime";
}

} // namespace iouring::observability
