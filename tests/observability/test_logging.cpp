#include <iouring_runtime/observability/Logging.h>

#include <gtest/gtest.h>

namespace {
using iouring_runtime::observability::LogLevel;
using iouring_runtime::observability::ParseLogLevel;
using iouring_runtime::observability::ConfigureLogging;
using iouring_runtime::observability::ShouldLog;

TEST(ObservabilityLoggingTest, ParsesKnownLogLevels) {
    EXPECT_EQ(ParseLogLevel("trace"), LogLevel::kTrace);
    EXPECT_EQ(ParseLogLevel("DEBUG"), LogLevel::kDebug);
    EXPECT_EQ(ParseLogLevel("info"), LogLevel::kInfo);
    EXPECT_EQ(ParseLogLevel("warning"), LogLevel::kWarn);
    EXPECT_EQ(ParseLogLevel("err"), LogLevel::kError);
    EXPECT_EQ(ParseLogLevel("fatal"), LogLevel::kCritical);
    EXPECT_EQ(ParseLogLevel("none"), LogLevel::kOff);
}

TEST(ObservabilityLoggingTest, RejectsUnknownLogLevel) {
    EXPECT_FALSE(ParseLogLevel("verbose").has_value());
    EXPECT_FALSE(ParseLogLevel("").has_value());
}

TEST(ObservabilityLoggingTest, ReportsEnabledLevels) {
    ConfigureLogging({.level = LogLevel::kWarn,
                      .include_timestamp = false,
                      .include_thread_id = false});

    EXPECT_FALSE(ShouldLog(LogLevel::kDebug));
    EXPECT_TRUE(ShouldLog(LogLevel::kWarn));
    EXPECT_TRUE(ShouldLog(LogLevel::kError));
    EXPECT_FALSE(ShouldLog(LogLevel::kOff));

    ConfigureLogging({.level = LogLevel::kInfo,
                      .include_timestamp = false,
                      .include_thread_id = false});
}

} // namespace
