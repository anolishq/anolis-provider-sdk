#include "anolis/provider_sdk/logging.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace anolis::provider_sdk::logging {
namespace {

std::atomic<LogLevel> g_threshold{LogLevel::Info};
std::mutex g_sink_mutex;

int level_rank(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return 0;
        case LogLevel::Info:
            return 1;
        case LogLevel::Warn:
            return 2;
        case LogLevel::Error:
            return 3;
        case LogLevel::None:
            return 4;
    }
    return 1;
}

std::string upper_copy(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

}  // namespace

void Logger::init(LogLevel threshold) { g_threshold.store(threshold); }

void Logger::init_from_env(const char* env_var) {
    init(LogLevel::Info);
    if (env_var == nullptr || *env_var == '\0') {
        return;
    }

    const char* raw = std::getenv(env_var);
    if (raw == nullptr || *raw == '\0') {
        return;
    }

    bool ok = false;
    const LogLevel parsed = parse_level(raw, &ok);
    set_level(parsed);

    if (!ok) {
        Logger::log(LogLevel::Warn, "Logger", __FILE__, __LINE__,
                    "Invalid " + std::string(env_var) + "='" + raw + "', defaulting to INFO");
    }
}

void Logger::set_level(LogLevel threshold) { g_threshold.store(threshold); }

LogLevel Logger::level() { return g_threshold.load(); }

LogLevel Logger::parse_level(const std::string& value, bool* ok) {
    const auto normalized = upper_copy(value);

    if (normalized == "DEBUG") {
        if (ok != nullptr) {
            *ok = true;
        }
        return LogLevel::Debug;
    }
    if (normalized == "INFO") {
        if (ok != nullptr) {
            *ok = true;
        }
        return LogLevel::Info;
    }
    if (normalized == "WARN" || normalized == "WARNING") {
        if (ok != nullptr) {
            *ok = true;
        }
        return LogLevel::Warn;
    }
    if (normalized == "ERROR") {
        if (ok != nullptr) {
            *ok = true;
        }
        return LogLevel::Error;
    }
    if (normalized == "NONE" || normalized == "OFF") {
        if (ok != nullptr) {
            *ok = true;
        }
        return LogLevel::None;
    }

    if (ok != nullptr) {
        *ok = false;
    }
    return LogLevel::Info;
}

const char* Logger::to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::None:
            return "NONE";
    }
    return "INFO";
}

void Logger::log(LogLevel level, const std::string& component, const char* file, int line, const std::string& message) {
    (void)file;
    (void)line;

    const LogLevel threshold = g_threshold.load();
    if (level_rank(level) < level_rank(threshold)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif

    std::lock_guard<std::mutex> lock(g_sink_mutex);
    std::cerr << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0') << std::setw(3)
              << ms.count() << "] "
              << "[" << to_string(level) << "] "
              << "[" << (component.empty() ? "General" : component) << "] " << message << "\n";

    if (level_rank(level) >= level_rank(LogLevel::Error)) {
        std::cerr << std::flush;
    }
}

}  // namespace anolis::provider_sdk::logging
