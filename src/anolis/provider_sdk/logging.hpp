#pragma once

// Structured, level-thresholded, thread-safe logging to stderr (spine).
//
// Lifted from sim's Logger (the richest of the three — ezo/bread had minimal
// free-function stubs that converge onto this during migration). Output is
// `[timestamp] [LEVEL] [component] message` on stderr, gated by a global
// threshold settable directly or from an environment variable.

#include <sstream>
#include <string>

namespace anolis::provider_sdk::logging {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
    None,
};

class Logger {
public:
    static void init(LogLevel threshold = LogLevel::Info);
    // Initialize the threshold from `env_var` (default INFO if unset/empty/invalid).
    // The provider passes its own variable name (e.g. ANOLIS_PROVIDER_SIM_LOG_LEVEL).
    static void init_from_env(const char* env_var = "ANOLIS_PROVIDER_LOG_LEVEL");
    static void set_level(LogLevel threshold);
    static LogLevel level();

    // Parse a level name (case-insensitive; WARN/WARNING and NONE/OFF accepted).
    // Sets `*ok` (if non-null) to false on an unrecognized value (returns Info).
    static LogLevel parse_level(const std::string& value, bool* ok = nullptr);
    static const char* to_string(LogLevel level);

    static void log(LogLevel level, const std::string& component, const char* file, int line,
                    const std::string& message);
};

}  // namespace anolis::provider_sdk::logging

// Ergonomic logging macros: capture __FILE__/__LINE__ and stream both `component`
// and `msg` through ostringstream so call sites can use `<<` expressions.
#define ANOLIS_PROVIDER_LOG_INTERNAL(level, component, msg)                                                     \
    do {                                                                                                        \
        std::ostringstream _anolis_log_component_ss;                                                            \
        _anolis_log_component_ss << component;                                                                  \
        std::ostringstream _anolis_log_message_ss;                                                              \
        _anolis_log_message_ss << msg;                                                                          \
        ::anolis::provider_sdk::logging::Logger::log(level, _anolis_log_component_ss.str(), __FILE__, __LINE__, \
                                                     _anolis_log_message_ss.str());                             \
    } while (0)

#define ANOLIS_PROVIDER_LOG_DEBUG(component, msg) \
    ANOLIS_PROVIDER_LOG_INTERNAL(::anolis::provider_sdk::logging::LogLevel::Debug, component, msg)
#define ANOLIS_PROVIDER_LOG_INFO(component, msg) \
    ANOLIS_PROVIDER_LOG_INTERNAL(::anolis::provider_sdk::logging::LogLevel::Info, component, msg)
#define ANOLIS_PROVIDER_LOG_WARN(component, msg) \
    ANOLIS_PROVIDER_LOG_INTERNAL(::anolis::provider_sdk::logging::LogLevel::Warn, component, msg)
#define ANOLIS_PROVIDER_LOG_ERROR(component, msg) \
    ANOLIS_PROVIDER_LOG_INTERNAL(::anolis::provider_sdk::logging::LogLevel::Error, component, msg)
