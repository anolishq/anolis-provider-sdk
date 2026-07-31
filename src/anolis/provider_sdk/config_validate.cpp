#include "anolis/provider_sdk/config_validate.hpp"

// Part 2 of the declare-once config toolkit: the YAML validator + the typed
// extraction helpers, both driven by the SAME YAML-1.2-core scalar resolver so
// what `validate()` accepts and what extraction converts cannot diverge.

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <string_view>

#include "anolis/provider_sdk/config_internal.hpp"

namespace anolis::provider_sdk::config {

namespace {

// ---- YAML 1.2 core-schema scalar resolution --------------------------------

struct Resolved {
    ScalarKind kind = ScalarKind::NonScalar;
    bool bool_value = false;
    std::int64_t int_value = 0;
    double float_value = 0.0;
    std::string string_value;  // engaged when kind == String
};

bool is_core_null(std::string_view text) {
    return text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL";
}

std::optional<bool> core_bool(std::string_view text) {
    if (text == "true" || text == "True" || text == "TRUE") {
        return true;
    }
    if (text == "false" || text == "False" || text == "FALSE") {
        return false;
    }
    return std::nullopt;
}

const std::regex& int_dec_pattern() {
    static const std::regex pattern(R"(^[-+]?[0-9]+$)");
    return pattern;
}

const std::regex& int_oct_pattern() {
    static const std::regex pattern(R"(^0o[0-7]+$)");
    return pattern;
}

const std::regex& int_hex_pattern() {
    static const std::regex pattern(R"(^0x[0-9a-fA-F]+$)");
    return pattern;
}

const std::regex& float_pattern() {
    static const std::regex pattern(R"(^[-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?$)");
    return pattern;
}

const std::regex& float_special_pattern() {
    static const std::regex pattern(R"(^([-+]?\.(inf|Inf|INF)|\.(nan|NaN|NAN))$)");
    return pattern;
}

std::optional<std::int64_t> parse_int(std::string_view text, int base, std::size_t skip_prefix) {
    std::int64_t value = 0;
    const char* first = text.data() + skip_prefix;
    const char* last = text.data() + text.size();
    if (skip_prefix > 0 && (text[0] == '+' || text[0] == '-')) {
        // Only the 0x/0o forms skip a prefix, and those carry no sign.
        return std::nullopt;
    }
    if (base == 10 && !text.empty() && text[0] == '+') {
        first = text.data() + 1;  // from_chars accepts '-' but not '+'
    }
    const auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc() || ptr != last) {
        return std::nullopt;  // out of int64 range (or malformed)
    }
    return value;
}

// Core-schema resolution of the raw text of a PLAIN scalar.
Resolved resolve_plain(const std::string& raw) {
    Resolved out;
    if (is_core_null(raw)) {
        out.kind = ScalarKind::Null;
        return out;
    }
    if (const auto b = core_bool(raw)) {
        out.kind = ScalarKind::Bool;
        out.bool_value = *b;
        return out;
    }
    if (std::regex_match(raw, int_dec_pattern())) {
        if (const auto v = parse_int(raw, 10, 0)) {
            out.kind = ScalarKind::Int;
            out.int_value = *v;
            return out;
        }
        // Out-of-int64-range integer text: fall through to float resolution
        // (the core schema has no bignum), then to string.
    }
    if (std::regex_match(raw, int_hex_pattern())) {
        if (const auto v = parse_int(raw, 16, 2)) {
            out.kind = ScalarKind::Int;
            out.int_value = *v;
            return out;
        }
    }
    if (std::regex_match(raw, int_oct_pattern())) {
        if (const auto v = parse_int(raw, 8, 2)) {
            out.kind = ScalarKind::Int;
            out.int_value = *v;
            return out;
        }
    }
    if (std::regex_match(raw, float_special_pattern())) {
        // stod does not accept the core-schema specials directly. The inf
        // forms (.inf/.Inf/.INF) end in f/F; the nan forms end in n/N.
        const bool is_inf = raw.back() == 'f' || raw.back() == 'F';
        if (!is_inf) {
            out.float_value = std::numeric_limits<double>::quiet_NaN();
        } else if (raw[0] == '-') {
            out.float_value = -std::numeric_limits<double>::infinity();
        } else {
            out.float_value = std::numeric_limits<double>::infinity();
        }
        out.kind = ScalarKind::Float;
        return out;
    }
    if (std::regex_match(raw, float_pattern())) {
        // std::from_chars, NOT std::stod: stod honors LC_NUMERIC, so a host
        // calling setlocale() would change what configs mean (under de_DE,
        // "5.7" stops parsing). The SDK is a library; parsing must be
        // locale-independent.
        const bool negative = raw[0] == '-';
        const char* first = raw.data() + (raw[0] == '+' || raw[0] == '-' ? 1 : 0);
        const char* last = raw.data() + raw.size();
        double parsed = 0.0;
        const auto [ptr, ec] = std::from_chars(first, last, parsed);
        if (ec == std::errc() && ptr == last) {
            out.float_value = negative ? -parsed : parsed;
            out.kind = ScalarKind::Float;
            return out;
        }
        if (ec == std::errc::result_out_of_range && ptr == last) {
            // Overflow (1e999) -> ±inf, which a Number field rejects as
            // non-finite; underflow (1e-999) -> ±0 like strtod. Discriminate
            // by the exponent's sign (no exponent: "0."-prefixed mantissas
            // underflow, huge digit strings overflow).
            std::string_view magnitude(first, static_cast<std::size_t>(last - first));
            bool underflow = false;
            if (const auto epos = magnitude.find_first_of("eE"); epos != std::string_view::npos) {
                underflow = epos + 1 < magnitude.size() && magnitude[epos + 1] == '-';
            } else {
                underflow = magnitude.substr(0, 2) == "0.";
            }
            if (underflow) {
                out.float_value = negative ? -0.0 : 0.0;
            } else {
                out.float_value =
                    negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            }
            out.kind = ScalarKind::Float;
            return out;
        }
        // A parse failure is unreachable behind the regex gate; fall through.
    }
    out.kind = ScalarKind::String;
    out.string_value = raw;
    return out;
}

Resolved resolve_scalar(const YAML::Node& node) {
    Resolved out;
    if (!node.IsDefined()) {
        return out;
    }
    if (node.IsNull()) {
        out.kind = ScalarKind::Null;
        return out;
    }
    if (!node.IsScalar()) {
        return out;
    }
    const std::string& raw = node.Scalar();
    const std::string& tag = node.Tag();
    // Quoted / block scalars (yaml-cpp tag "!") and explicit !!str are strings
    // regardless of content: `"300"` is NOT the integer 300.
    if (tag == "!" || tag == "tag:yaml.org,2002:str") {
        out.kind = ScalarKind::String;
        out.string_value = raw;
        return out;
    }
    return resolve_plain(raw);
}

const char* kind_name(ScalarKind kind) {
    switch (kind) {
        case ScalarKind::Null:
            return "null";
        case ScalarKind::Bool:
            return "a boolean";
        case ScalarKind::Int:
            return "an integer";
        case ScalarKind::Float:
            return "a number";
        case ScalarKind::String:
            return "a string";
        case ScalarKind::NonScalar:
            return "not a scalar";
    }
    return "unknown";
}

// JSON Schema `integer` semantics: a true int, or a float with zero fraction.
// Accepts every integral double representable in int64 (the cast is exact
// there); integral doubles at/above 2^63 stay rejected — a documented
// deviation from a pure JSON Schema validator, which has no int64 cap.
std::optional<std::int64_t> integer_of(const Resolved& resolved) {
    if (resolved.kind == ScalarKind::Int) {
        return resolved.int_value;
    }
    if (resolved.kind == ScalarKind::Float) {
        const double v = resolved.float_value;
        if (std::isfinite(v) && v == std::floor(v) && v >= -9223372036854775808.0 && v < 9223372036854775808.0) {
            return static_cast<std::int64_t>(v);
        }
    }
    return std::nullopt;
}

std::optional<int> i2c_address_of(const Resolved& resolved) {
    std::int64_t value = 0;
    // The numeric branch mirrors the emitted `type: integer` exactly, so a
    // zero-fraction float (8.0) is an address, matching `integer` elsewhere.
    if (const auto as_integer = integer_of(resolved)) {
        value = *as_integer;
    } else if (resolved.kind == ScalarKind::String) {
        static const std::regex hex_pattern(internal::kI2cHexPattern);
        if (!std::regex_match(resolved.string_value, hex_pattern)) {
            return std::nullopt;
        }
        const auto parsed = parse_int(resolved.string_value, 16, 2);
        if (!parsed.has_value()) {
            return std::nullopt;  // unreachable behind the pattern gate
        }
        value = parsed.value();
    } else {
        return std::nullopt;
    }
    if (value < internal::kI2cAddressMin || value > internal::kI2cAddressMax) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

// ---- validation ------------------------------------------------------------

void add_error(std::vector<ValidationError>& errors, std::string path, std::string message) {
    errors.push_back({std::move(path), std::move(message)});
}

std::string child_path(const std::string& base, const std::string& key) {
    return base.empty() ? key : base + "." + key;
}

std::string index_path(const std::string& base, std::size_t index) { return std::format("{}[{}]", base, index); }

void validate_field(const Field::Spec& spec, const YAML::Node& node, const std::string& path,
                    std::vector<ValidationError>& errors);
void validate_object(const Object::Spec& spec, const YAML::Node& node, const std::string& path,
                     std::vector<ValidationError>& errors);
void validate_array(const Array::Spec& spec, const YAML::Node& node, const std::string& path,
                    std::vector<ValidationError>& errors);

void validate_string_constraints(const Field::Spec& spec, const std::string& value, const std::string& path,
                                 std::vector<ValidationError>& errors) {
    if (spec.non_empty && value.empty()) {
        add_error(errors, path, "must not be empty");
    }
    if (spec.const_value && value != *spec.const_value) {
        add_error(errors, path, std::format("must be '{}'", *spec.const_value));
        return;  // the const already names the only accepted value
    }
    if (!spec.enum_values.empty()) {
        bool found = false;
        for (const auto& ev : spec.enum_values) {
            found = found || ev.value == value;
        }
        if (!found) {
            std::string allowed;
            for (const auto& ev : spec.enum_values) {
                if (!allowed.empty()) {
                    allowed += ", ";
                }
                allowed += ev.value;
            }
            add_error(errors, path, std::format("invalid value '{}'; valid values: {}", value, allowed));
            return;
        }
    }
    if (spec.pattern && !std::regex_search(value, std::regex(*spec.pattern, std::regex::ECMAScript))) {
        add_error(errors, path, std::format("must match {}", *spec.pattern));
    }
    for (const auto& forbidden : spec.forbidden_values) {
        if (forbidden.value == value) {
            add_error(errors, path,
                      forbidden.message.empty() ? std::format("value '{}' is not allowed", value) : forbidden.message);
        }
    }
}

void validate_field(const Field::Spec& spec, const YAML::Node& node, const std::string& path,
                    std::vector<ValidationError>& errors) {
    const Resolved resolved = resolve_scalar(node);
    switch (spec.kind) {
        case Field::Kind::String: {
            if (resolved.kind != ScalarKind::String) {
                add_error(errors, path, std::format("must be a string, got {}", kind_name(resolved.kind)));
                return;
            }
            validate_string_constraints(spec, resolved.string_value, path, errors);
            break;
        }
        case Field::Kind::Integer: {
            const auto value = integer_of(resolved);
            if (!value) {
                add_error(errors, path, std::format("must be an integer, got {}", kind_name(resolved.kind)));
                return;
            }
            if (spec.min_int && *value < *spec.min_int) {
                add_error(errors, path, std::format("must be >= {}", *spec.min_int));
            }
            if (spec.max_int && *value > *spec.max_int) {
                add_error(errors, path, std::format("must be <= {}", *spec.max_int));
            }
            break;
        }
        case Field::Kind::Number: {
            double value = 0.0;
            if (resolved.kind == ScalarKind::Int) {
                value = static_cast<double>(resolved.int_value);
            } else if (resolved.kind == ScalarKind::Float) {
                value = resolved.float_value;
            } else {
                add_error(errors, path, std::format("must be a number, got {}", kind_name(resolved.kind)));
                return;
            }
            // JSON cannot represent .inf/.nan, so a `type: number` field
            // honestly rejects them.
            if (!std::isfinite(value)) {
                add_error(errors, path, "must be a finite number");
                return;
            }
            if (spec.min_number && value < *spec.min_number) {
                add_error(errors, path, std::format("must be >= {}", *spec.min_number));
            }
            if (spec.exclusive_min_number && value <= *spec.exclusive_min_number) {
                add_error(errors, path, std::format("must be > {}", *spec.exclusive_min_number));
            }
            if (spec.max_number && value > *spec.max_number) {
                add_error(errors, path, std::format("must be <= {}", *spec.max_number));
            }
            if (spec.exclusive_max_number && value >= *spec.exclusive_max_number) {
                add_error(errors, path, std::format("must be < {}", *spec.exclusive_max_number));
            }
            break;
        }
        case Field::Kind::Boolean: {
            if (resolved.kind != ScalarKind::Bool) {
                add_error(errors, path,
                          std::format("must be a boolean (true/false), got {}", kind_name(resolved.kind)));
            }
            break;
        }
        case Field::Kind::I2cAddress: {
            // A zero-fraction float (8.0) is integer-like — mirrors the
            // emitted `type: integer` branch exactly.
            const bool integer_like = resolved.kind == ScalarKind::Int ||
                                      (resolved.kind == ScalarKind::Float && integer_of(resolved).has_value());
            if (!integer_like && resolved.kind != ScalarKind::String) {
                add_error(
                    errors, path,
                    std::format("must be an integer or 0x-prefixed hex string, got {}", kind_name(resolved.kind)));
                return;
            }
            if (!i2c_address_of(resolved)) {
                add_error(errors, path, "must be an I2C address in the 0x08-0x77 range (integer or 0xNN string)");
            }
            break;
        }
    }
}

// The canonical parsed value used for uniqueness comparisons — `0x08` and
// `"0x08"` collide on an I2cAddress field even though they differ as JSON.
// nullopt when the node does not satisfy the field's type (already reported).
std::optional<std::string> canonical_value(const Field::Spec& spec, const YAML::Node& node) {
    const Resolved resolved = resolve_scalar(node);
    switch (spec.kind) {
        case Field::Kind::String:
            if (resolved.kind == ScalarKind::String) {
                return "s:" + resolved.string_value;
            }
            return std::nullopt;
        case Field::Kind::Integer: {
            const auto value = integer_of(resolved);
            return value ? std::optional<std::string>("i:" + std::to_string(*value)) : std::nullopt;
        }
        case Field::Kind::Number: {
            if (resolved.kind == ScalarKind::Int) {
                return "f:" + std::format("{}", static_cast<double>(resolved.int_value));
            }
            if (resolved.kind == ScalarKind::Float && std::isfinite(resolved.float_value)) {
                // Normalize -0.0 to 0.0: uniqueItems treats them as equal.
                const double v = resolved.float_value == 0.0 ? 0.0 : resolved.float_value;
                return "f:" + std::format("{}", v);
            }
            return std::nullopt;
        }
        case Field::Kind::Boolean:
            return resolved.kind == ScalarKind::Bool ? std::optional<std::string>(resolved.bool_value ? "b:1" : "b:0")
                                                     : std::nullopt;
        case Field::Kind::I2cAddress: {
            const auto value = i2c_address_of(resolved);
            return value ? std::optional<std::string>("i:" + std::to_string(*value)) : std::nullopt;
        }
    }
    return std::nullopt;
}

void validate_array(const Array::Spec& spec, const YAML::Node& node, const std::string& path,
                    std::vector<ValidationError>& errors) {
    if (!node.IsSequence()) {
        add_error(errors, path, "must be a sequence");
        return;
    }
    const std::size_t size = node.size();
    if (spec.min_items && size < *spec.min_items) {
        add_error(errors, path, std::format("must have at least {} item(s)", *spec.min_items));
    }
    if (spec.max_items && size > *spec.max_items) {
        add_error(errors, path, std::format("must have at most {} item(s)", *spec.max_items));
    }

    if (spec.item_field) {
        const Field::Spec& item = spec.item_field->spec();
        std::set<std::string> seen;
        for (std::size_t i = 0; i < size; ++i) {
            const std::string item_path = index_path(path, i);
            validate_field(item, node[i], item_path, errors);
            if (spec.unique) {
                if (const auto canonical = canonical_value(item, node[i])) {
                    if (!seen.insert(*canonical).second) {
                        add_error(errors, item_path, "duplicate value");
                    }
                }
            }
        }
        return;
    }

    const Object::Spec& item = spec.item_object->spec();
    // Per-field uniqueness across items (e.g. devices[].id / devices[].address).
    std::map<std::string, std::set<std::string>> seen_by_key;
    for (std::size_t i = 0; i < size; ++i) {
        const std::string item_path = index_path(path, i);
        validate_object(item, node[i], item_path, errors);
        if (!node[i].IsMap()) {
            continue;
        }
        for (const auto& member : item.members) {
            if (member.kind != Object::Member::Kind::Scalar || !member.field.has_value() ||
                !member.field->spec().unique) {
                continue;
            }
            const YAML::Node value_node = node[i][member.key];
            if (!value_node.IsDefined()) {
                continue;
            }
            if (const auto canonical = canonical_value(member.field->spec(), value_node)) {
                if (!seen_by_key[member.key].insert(*canonical).second) {
                    add_error(
                        errors, child_path(item_path, member.key),
                        std::format("duplicate {} '{}'", member.key, value_node.IsScalar() ? value_node.Scalar() : ""));
                }
            }
        }
    }
}

void validate_object(const Object::Spec& spec, const YAML::Node& node, const std::string& path,
                     std::vector<ValidationError>& errors) {
    if (!node.IsMap()) {
        add_error(errors, path.empty() ? "(root)" : path, "must be a map");
        return;
    }

    std::set<std::string> declared;
    for (const auto& member : spec.members) {
        declared.insert(member.key);
    }

    // Present keys, in document order. A duplicated key is rejected outright:
    // yaml-cpp silently serves the FIRST occurrence while other YAML stacks
    // are last-wins (or throw), so validating either occurrence would bless an
    // ambiguous document.
    std::vector<std::string> present;
    std::set<std::string> present_set;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            add_error(errors, path.empty() ? "(root)" : path, "keys must be scalars");
            continue;
        }
        const std::string key = entry.first.Scalar();
        if (!present_set.insert(key).second) {
            add_error(errors, child_path(path, key), "duplicate key");
            continue;
        }
        present.push_back(key);
    }

    // Deprecated keys carry their own message; unknown keys are rejected only
    // on Closed objects.
    for (const auto& key : present) {
        bool deprecated = false;
        for (const auto& dep : spec.deprecated_keys) {
            if (dep.key == key) {
                add_error(errors, child_path(path, key), dep.message.empty() ? "is no longer supported" : dep.message);
                deprecated = true;
                break;
            }
        }
        if (!deprecated && spec.openness == Openness::Closed && declared.find(key) == declared.end()) {
            add_error(errors, child_path(path, key), "unknown key");
        }
    }

    for (const auto& member : spec.members) {
        const YAML::Node value = node[member.key];
        const std::string member_path = child_path(path, member.key);
        if (!value.IsDefined()) {
            if (member.required) {
                add_error(errors, member_path, "is required");
            }
            continue;
        }
        switch (member.kind) {
            case Object::Member::Kind::Scalar:
                if (member.field.has_value()) {  // always true for Kind::Scalar; lint guard
                    validate_field(member.field->spec(), value, member_path, errors);
                }
                break;
            case Object::Member::Kind::Object:
                validate_object(member.object->spec(), value, member_path, errors);
                break;
            case Object::Member::Kind::Array:
                validate_array(member.array->spec(), value, member_path, errors);
                break;
        }
    }

    // Discriminator conditionals: a branch applies when its discriminator is
    // present AND resolves to the branch's string value — mirroring the
    // emitted `if: {properties: ..., required: [...]}` exactly.
    for (const auto& conditional : spec.conditionals) {
        const YAML::Node disc = node[conditional.key];
        if (!disc.IsDefined()) {
            continue;
        }
        const Resolved resolved = resolve_scalar(disc);
        if (resolved.kind != ScalarKind::String || resolved.string_value != conditional.equals) {
            continue;
        }
        for (const auto& key : conditional.branch.require_keys()) {
            if (present_set.find(key) == present_set.end()) {
                add_error(errors, child_path(path, key),
                          std::format("is required when {} is '{}'", conditional.key, conditional.equals));
            }
        }
        for (const auto& key : conditional.branch.forbid_keys()) {
            if (present_set.find(key) != present_set.end()) {
                add_error(errors, child_path(path, key),
                          std::format("is not valid when {} is '{}'", conditional.key, conditional.equals));
            }
        }
    }

    for (const auto& dependent : spec.dependent_required) {
        if (present_set.find(dependent.key) == present_set.end()) {
            continue;
        }
        for (const auto& key : dependent.also_required) {
            if (present_set.find(key) == present_set.end()) {
                add_error(errors, child_path(path, key), std::format("is required when {} is present", dependent.key));
            }
        }
    }
}

}  // namespace

std::vector<ValidationError> validate(const Schema& schema, const YAML::Node& root) {
    std::vector<ValidationError> errors;
    validate_object(schema.root().spec(), root, "", errors);
    return errors;
}

std::string format_errors(const std::vector<ValidationError>& errors) {
    std::string out;
    for (const auto& error : errors) {
        if (!out.empty()) {
            out += '\n';
        }
        out += error.path;
        out += ": ";
        out += error.message;
    }
    return out;
}

ScalarKind resolved_kind(const YAML::Node& node) { return resolve_scalar(node).kind; }

std::optional<std::string> as_string(const YAML::Node& node) {
    const Resolved resolved = resolve_scalar(node);
    if (resolved.kind != ScalarKind::String) {
        return std::nullopt;
    }
    return resolved.string_value;
}

std::optional<std::int64_t> as_int64(const YAML::Node& node) { return integer_of(resolve_scalar(node)); }

std::optional<double> as_double(const YAML::Node& node) {
    const Resolved resolved = resolve_scalar(node);
    if (resolved.kind == ScalarKind::Int) {
        return static_cast<double>(resolved.int_value);
    }
    if (resolved.kind == ScalarKind::Float) {
        return resolved.float_value;
    }
    return std::nullopt;
}

std::optional<bool> as_bool(const YAML::Node& node) {
    const Resolved resolved = resolve_scalar(node);
    if (resolved.kind != ScalarKind::Bool) {
        return std::nullopt;
    }
    return resolved.bool_value;
}

std::optional<int> parse_i2c_address(const YAML::Node& node) { return i2c_address_of(resolve_scalar(node)); }

}  // namespace anolis::provider_sdk::config
