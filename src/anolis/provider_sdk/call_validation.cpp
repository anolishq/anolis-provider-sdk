#include "anolis/provider_sdk/call_validation.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <string>

#include "anolis/provider_sdk/echo_internal.hpp"

namespace anolis::provider_sdk {
namespace {

// The oneof field that carries a value of each declared type.
adpp::Value::KindCase field_for(adpp::ValueType type) {
    switch (type) {
        case adpp::VALUE_TYPE_BOOL:
            return adpp::Value::kBoolValue;
        case adpp::VALUE_TYPE_INT64:
            return adpp::Value::kInt64Value;
        case adpp::VALUE_TYPE_UINT64:
            return adpp::Value::kUint64Value;
        case adpp::VALUE_TYPE_DOUBLE:
            return adpp::Value::kDoubleValue;
        case adpp::VALUE_TYPE_STRING:
            return adpp::Value::kStringValue;
        case adpp::VALUE_TYPE_BYTES:
            return adpp::Value::kBytesValue;
        default:
            return adpp::Value::KIND_NOT_SET;
    }
}

std::string type_name(int type) {
    return adpp::ValueType_IsValid(type) ? adpp::ValueType_Name(static_cast<adpp::ValueType>(type))
                                         : std::to_string(type);
}

std::string quoted(const std::string& name) { return "'" + name + "'"; }

std::string number(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

ArgError invalid(const std::string& message) { return {adpp::Status::CODE_INVALID_ARGUMENT, message}; }

ArgError out_of_range(const std::string& message) { return {adpp::Status::CODE_OUT_OF_RANGE, message}; }

// Rule 3: the type tag is the declared type or unspecified, and the field set is
// the declared type's field or none; an unspecified tag with no field is empty.
std::optional<ArgError> check_type(const adpp::ArgSpec& arg, const adpp::Value& value) {
    const adpp::Value::KindCase expected = field_for(arg.type());
    if (expected == adpp::Value::KIND_NOT_SET) {
        return std::nullopt;  // a declared type the SDK cannot check
    }
    const bool tag_ok = value.type() == arg.type() || value.type() == adpp::VALUE_TYPE_UNSPECIFIED;
    const bool field_ok = value.kind_case() == expected || value.kind_case() == adpp::Value::KIND_NOT_SET;
    if (!tag_ok || !field_ok) {
        return invalid("argument " + quoted(arg.name()) + " expects " + type_name(arg.type()) + ", got " +
                       type_name(value.type()));
    }
    if (value.type() == adpp::VALUE_TYPE_UNSPECIFIED && value.kind_case() == adpp::Value::KIND_NOT_SET) {
        return invalid("argument " + quoted(arg.name()) + " expects " + type_name(arg.type()) + ", got an empty value");
    }
    return std::nullopt;
}

// Rules 4 and 5 for one argument whose type is already checked.
std::optional<ArgError> check_bounds(const adpp::ArgSpec& arg, const adpp::Value& value) {
    const std::string name = quoted(arg.name());
    switch (arg.type()) {
        case adpp::VALUE_TYPE_DOUBLE: {
            const double v = value.double_value();
            if (!std::isfinite(v)) {
                return invalid("argument " + name + " must be finite, got " + number(v));
            }
            if ((arg.has_min_double() && v < arg.min_double()) || (arg.has_max_double() && v > arg.max_double())) {
                return out_of_range("argument " + name + " = " + number(v) + " is outside its declared bounds");
            }
            return std::nullopt;
        }
        case adpp::VALUE_TYPE_INT64: {
            const int64_t v = value.int64_value();
            if ((arg.has_min_int64() && v < arg.min_int64()) || (arg.has_max_int64() && v > arg.max_int64())) {
                return out_of_range("argument " + name + " = " + std::to_string(v) + " is outside its declared bounds");
            }
            return std::nullopt;
        }
        case adpp::VALUE_TYPE_UINT64: {
            const uint64_t v = value.uint64_value();
            if ((arg.has_min_uint64() && v < arg.min_uint64()) || (arg.has_max_uint64() && v > arg.max_uint64())) {
                return out_of_range("argument " + name + " = " + std::to_string(v) + " is outside its declared bounds");
            }
            return std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

// Rule 6: allowed_values constrains string arguments; any other value is outside it.
std::optional<ArgError> check_allowed(const adpp::ArgSpec& arg, const adpp::Value& value) {
    if (arg.allowed_values().empty()) {
        return std::nullopt;
    }
    const bool allowed = arg.type() == adpp::VALUE_TYPE_STRING &&
                         std::find(arg.allowed_values().begin(), arg.allowed_values().end(), value.string_value()) !=
                             arg.allowed_values().end();
    if (allowed) {
        return std::nullopt;
    }
    std::string list;
    for (const auto& v : arg.allowed_values()) {
        list += (list.empty() ? "" : ", ") + quoted(v);
    }
    return invalid("argument " + quoted(arg.name()) + " must be one of [" + list + "]");
}

}  // namespace

std::optional<ArgError> validate_call_args(const adpp::FunctionSpec& spec, const ValueMap& args) {
    // Rule 1, reporting the first undeclared name in sorted order.
    std::set<std::string> declared;
    for (const auto& arg : spec.args()) {
        declared.insert(arg.name());
    }
    std::set<std::string> undeclared;
    for (const auto& [name, value] : args) {
        if (declared.count(name) == 0) {
            undeclared.insert(name);
        }
    }
    if (!undeclared.empty()) {
        // The name comes from the request, so its echo is bounded (#50).
        return invalid(spec.name() + " takes no argument " + quoted(internal::echo(*undeclared.begin())));
    }
    // Rules 2 to 6, argument by argument in declared order.
    for (const auto& arg : spec.args()) {
        const auto it = args.find(arg.name());
        if (it == args.end()) {
            if (arg.required()) {
                return invalid(spec.name() + " requires argument " + quoted(arg.name()));
            }
            continue;
        }
        if (auto err = check_type(arg, it->second)) {
            return err;
        }
        if (auto err = check_bounds(arg, it->second)) {
            return err;
        }
        if (auto err = check_allowed(arg, it->second)) {
            return err;
        }
    }
    return std::nullopt;
}

}  // namespace anolis::provider_sdk
