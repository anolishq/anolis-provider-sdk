#include "anolis/provider_sdk/logging.hpp"

#include <gtest/gtest.h>

#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>

// D.3a Logger tests: level parsing/formatting, threshold gating (via a cerr
// capture), and the env-init defaulting. Lifted from sim's Logger.

namespace log = anolis::provider_sdk::logging;
using log::Logger;
using log::LogLevel;

namespace {

// RAII redirect of std::cerr to a captured buffer for the test's duration.
class CerrCapture {
public:
    CerrCapture() : prev_(std::cerr.rdbuf(buffer_.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(prev_); }
    std::string str() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
    std::streambuf* prev_;
};

}  // namespace

TEST(LoggerTest, ParseLevelIsCaseInsensitiveWithAliases) {
    bool ok = false;
    EXPECT_EQ(Logger::parse_level("debug", &ok), LogLevel::Debug);
    EXPECT_TRUE(ok);
    EXPECT_EQ(Logger::parse_level("InFo", &ok), LogLevel::Info);
    EXPECT_EQ(Logger::parse_level("WARNING", &ok), LogLevel::Warn);  // alias
    EXPECT_EQ(Logger::parse_level("warn", &ok), LogLevel::Warn);
    EXPECT_EQ(Logger::parse_level("ERROR", &ok), LogLevel::Error);
    EXPECT_EQ(Logger::parse_level("off", &ok), LogLevel::None);  // alias
    EXPECT_TRUE(ok);
}

TEST(LoggerTest, ParseLevelRejectsUnknownAndDefaultsToInfo) {
    bool ok = true;
    EXPECT_EQ(Logger::parse_level("nonsense", &ok), LogLevel::Info);
    EXPECT_FALSE(ok);
}

TEST(LoggerTest, ToStringMatchesLevels) {
    EXPECT_STREQ(Logger::to_string(LogLevel::Debug), "DEBUG");
    EXPECT_STREQ(Logger::to_string(LogLevel::Info), "INFO");
    EXPECT_STREQ(Logger::to_string(LogLevel::Warn), "WARN");
    EXPECT_STREQ(Logger::to_string(LogLevel::Error), "ERROR");
    EXPECT_STREQ(Logger::to_string(LogLevel::None), "NONE");
}

TEST(LoggerTest, SetLevelRoundTrips) {
    Logger::set_level(LogLevel::Warn);
    EXPECT_EQ(Logger::level(), LogLevel::Warn);
    Logger::set_level(LogLevel::Info);
    EXPECT_EQ(Logger::level(), LogLevel::Info);
}

TEST(LoggerTest, ThresholdSuppressesBelowAndEmitsAtOrAbove) {
    Logger::set_level(LogLevel::Warn);
    {
        CerrCapture cap;
        Logger::log(LogLevel::Info, "comp", __FILE__, __LINE__, "should be suppressed");
        EXPECT_TRUE(cap.str().empty()) << "Info below the Warn threshold must not emit";
    }
    {
        CerrCapture cap;
        Logger::log(LogLevel::Error, "comp", __FILE__, __LINE__, "boom");
        const std::string out = cap.str();
        EXPECT_NE(out.find("[ERROR]"), std::string::npos);
        EXPECT_NE(out.find("[comp]"), std::string::npos);
        EXPECT_NE(out.find("boom"), std::string::npos);
    }
    Logger::set_level(LogLevel::Info);
}

TEST(LoggerTest, EmptyComponentRendersAsGeneral) {
    Logger::set_level(LogLevel::Info);
    CerrCapture cap;
    Logger::log(LogLevel::Info, "", __FILE__, __LINE__, "msg");
    EXPECT_NE(cap.str().find("[General]"), std::string::npos);
}

TEST(LoggerTest, InitFromEnvDefaultsToInfoWhenUnset) {
    Logger::set_level(LogLevel::Error);
    Logger::init_from_env("ANOLIS_PROVIDER_SDK_DEFINITELY_UNSET_VAR");
    EXPECT_EQ(Logger::level(), LogLevel::Info) << "unset var must default to INFO";

    Logger::set_level(LogLevel::Error);
    Logger::init_from_env(nullptr);
    EXPECT_EQ(Logger::level(), LogLevel::Info);
}

TEST(LoggerTest, MacrosCompileAndRespectThreshold) {
    Logger::set_level(LogLevel::Error);
    CerrCapture cap;
    ANOLIS_PROVIDER_LOG_INFO("macros", "suppressed " << 1 + 1);
    ANOLIS_PROVIDER_LOG_ERROR("macros", "emitted " << 42);
    const std::string out = cap.str();
    EXPECT_EQ(out.find("suppressed"), std::string::npos);
    EXPECT_NE(out.find("emitted 42"), std::string::npos);
    Logger::set_level(LogLevel::Info);
}
