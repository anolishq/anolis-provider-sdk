#pragma once

// Value / argument / signal helpers shared by device adapters — the union of the
// providers' helper sets (D.2). bread contributes the full five-type value
// makers + `get_arg_uint64`; sim contributes `make_arg_spec` /
// `make_function_policy`; `make_signal_value` is common. Provider-local wire
// helpers (bread's little-endian payload byte builders, its CRUMBS session error
// mappers) stay below the descriptor and are NOT lifted here.

// Prevent Windows macros (min/max, GetCurrentTime) from polluting the namespace.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <google/protobuf/util/time_util.h>

#include <cstdint>
#include <string>

#include "anolis/provider_sdk/result.hpp"  // adpp alias + ValueMap
#include "protocol.pb.h"

namespace anolis::provider_sdk {

// --- typed argument getters (return false on missing key or type mismatch) ---

inline bool get_arg_bool(const ValueMap& args, const std::string& key, bool& out) {
    const auto it = args.find(key);
    if (it == args.end() || it->second.type() != adpp::VALUE_TYPE_BOOL) return false;
    out = it->second.bool_value();
    return true;
}

inline bool get_arg_int64(const ValueMap& args, const std::string& key, int64_t& out) {
    const auto it = args.find(key);
    if (it == args.end() || it->second.type() != adpp::VALUE_TYPE_INT64) return false;
    out = it->second.int64_value();
    return true;
}

inline bool get_arg_uint64(const ValueMap& args, const std::string& key, uint64_t& out) {
    const auto it = args.find(key);
    if (it == args.end() || it->second.type() != adpp::VALUE_TYPE_UINT64) return false;
    out = it->second.uint64_value();
    return true;
}

inline bool get_arg_double(const ValueMap& args, const std::string& key, double& out) {
    const auto it = args.find(key);
    if (it == args.end() || it->second.type() != adpp::VALUE_TYPE_DOUBLE) return false;
    out = it->second.double_value();
    return true;
}

inline bool get_arg_string(const ValueMap& args, const std::string& key, std::string& out) {
    const auto it = args.find(key);
    if (it == args.end() || it->second.type() != adpp::VALUE_TYPE_STRING) return false;
    out = it->second.string_value();
    return true;
}

// --- typed value builders ---

inline adpp::Value make_bool_val(bool b) {
    adpp::Value v;
    v.set_type(adpp::VALUE_TYPE_BOOL);
    v.set_bool_value(b);
    return v;
}

inline adpp::Value make_int64_val(int64_t i) {
    adpp::Value v;
    v.set_type(adpp::VALUE_TYPE_INT64);
    v.set_int64_value(i);
    return v;
}

inline adpp::Value make_uint64_val(uint64_t u) {
    adpp::Value v;
    v.set_type(adpp::VALUE_TYPE_UINT64);
    v.set_uint64_value(u);
    return v;
}

inline adpp::Value make_double_val(double d) {
    adpp::Value v;
    v.set_type(adpp::VALUE_TYPE_DOUBLE);
    v.set_double_value(d);
    return v;
}

inline adpp::Value make_string_val(const std::string& s) {
    adpp::Value v;
    v.set_type(adpp::VALUE_TYPE_STRING);
    v.set_string_value(s);
    return v;
}

// Build a `SignalValue` stamped now with `QUALITY_OK`. The read path overrides
// the quality via `quality_from` (quality.hpp) where it has the inputs.
inline adpp::SignalValue make_signal_value(const std::string& id, const adpp::Value& value) {
    adpp::SignalValue sv;
    sv.set_signal_id(id);
    *sv.mutable_value() = value;
    *sv.mutable_timestamp() = (google::protobuf::util::TimeUtil::GetCurrentTime)();
    sv.set_quality(adpp::SignalValue::QUALITY_OK);
    return sv;
}

// --- capability-declaration builders (sim's basis) ---

inline adpp::ArgSpec make_arg_spec(const std::string& name, adpp::ValueType type, bool required,
                                   const std::string& description = "", const std::string& unit = "") {
    adpp::ArgSpec arg;
    arg.set_name(name);
    arg.set_type(type);
    arg.set_required(required);
    arg.set_description(description);
    arg.set_unit(unit);
    return arg;
}

inline adpp::FunctionPolicy make_function_policy(adpp::FunctionPolicy::Category category, bool requires_lease = false,
                                                 bool is_idempotent = false, int32_t min_interval_ms = 0) {
    adpp::FunctionPolicy policy;
    policy.set_category(category);
    policy.set_requires_lease(requires_lease);
    policy.set_is_idempotent(is_idempotent);
    policy.set_min_interval_ms(min_interval_ms);
    return policy;
}

}  // namespace anolis::provider_sdk
