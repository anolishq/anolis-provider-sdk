#include "anolis/provider_sdk/config.hpp"

#include "anolis/provider_sdk/config_internal.hpp"

// Implementation notes:
//
// - The JSON writer is deliberately in-house and minimal: the SDK needs
//   deterministic declaration-order keys (golden tests, diffable output),
//   int64 fidelity (`config_schema_version: 1` must parse as an *integer*
//   downstream — the conformance validator rejects floats), and an explicit
//   non-finite policy. A generic JSON dependency buys none of that.
// - Declaration-tree checks live in `Schema`'s constructor so a provider's
//   schema bug fails its unit tests at construction, with a dotted path.

#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <variant>

namespace anolis::provider_sdk::config {

namespace {

// ---- minimal deterministic JSON model ------------------------------------

struct JsonValue;
using JObject = std::vector<std::pair<std::string, JsonValue>>;
using JArray = std::vector<JsonValue>;

struct JsonValue {
    std::variant<bool, std::int64_t, double, std::string, JObject, JArray> value;

    JsonValue(bool v) : value(v) {}                            // NOLINT(google-explicit-constructor)
    JsonValue(int v) : value(static_cast<std::int64_t>(v)) {}  // NOLINT(google-explicit-constructor)
    JsonValue(std::int64_t v) : value(v) {}                    // NOLINT(google-explicit-constructor)
    JsonValue(double v) : value(v) {}                          // NOLINT(google-explicit-constructor)
    JsonValue(const char* v) : value(std::string(v)) {}        // NOLINT(google-explicit-constructor)
    JsonValue(std::string v) : value(std::move(v)) {}          // NOLINT(google-explicit-constructor)
    JsonValue(JObject v) : value(std::move(v)) {}              // NOLINT(google-explicit-constructor)
    JsonValue(JArray v) : value(std::move(v)) {}               // NOLINT(google-explicit-constructor)
};

std::string escape_json(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", c);
                } else {
                    out.push_back(raw);  // UTF-8 passes through byte-for-byte
                }
        }
    }
    out.push_back('"');
    return out;
}

void write_json(std::string& out, const JsonValue& value, int depth) {
    const auto indent = [&out](int d) { out.append(static_cast<std::size_t>(d) * 2, ' '); };

    if (const auto* b = std::get_if<bool>(&value.value)) {
        out += *b ? "true" : "false";
    } else if (const auto* integer = std::get_if<std::int64_t>(&value.value)) {
        out += std::to_string(*integer);
    } else if (const auto* d = std::get_if<double>(&value.value)) {
        // Setters reject non-finite bounds, so every stored double serializes.
        out += std::format("{}", *d);
    } else if (const auto* s = std::get_if<std::string>(&value.value)) {
        out += escape_json(*s);
    } else if (const auto* obj = std::get_if<JObject>(&value.value)) {
        if (obj->empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        for (std::size_t i = 0; i < obj->size(); ++i) {
            indent(depth + 1);
            out += escape_json((*obj)[i].first);
            out += ": ";
            write_json(out, (*obj)[i].second, depth + 1);
            if (i + 1 < obj->size()) {
                out.push_back(',');
            }
            out.push_back('\n');
        }
        indent(depth);
        out.push_back('}');
    } else {
        const auto& arr = std::get<JArray>(value.value);
        if (arr.empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < arr.size(); ++i) {
            indent(depth + 1);
            write_json(out, arr[i], depth + 1);
            if (i + 1 < arr.size()) {
                out.push_back(',');
            }
            out.push_back('\n');
        }
        indent(depth);
        out.push_back(']');
    }
}

std::string serialize(const JsonValue& value) {
    std::string out;
    write_json(out, value, 0);
    return out;
}

// ---- built-in I2cAddress constants ---------------------------------------
// Shared with the validator (config_validate.cpp) via config_internal.hpp so
// the emitted schema and the enforced range cannot disagree.

using internal::kI2cAddressMax;
using internal::kI2cAddressMin;
using internal::kI2cHexPattern;

// ---- declaration-tree checks ---------------------------------------------

[[noreturn]] void tree_error(const std::string& path, const std::string& message) {
    throw std::logic_error("config schema: " + path + ": " + message);
}

// Strict UTF-8 well-formedness (rejects overlongs, surrogates, > U+10FFFF).
// Invalid bytes would make the emitted document not-JSON, so they must fail at
// declaration time, not at the downstream conformance gate.
bool is_valid_utf8(const std::string& text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t size = text.size();
    std::size_t i = 0;
    while (i < size) {
        const unsigned char lead = bytes[i];
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char first_lo = 0x80;
        unsigned char first_hi = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            first_lo = 0xA0;
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            first_hi = 0x9F;  // exclude surrogates
        } else if (lead == 0xF0) {
            length = 4;
            first_lo = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            first_hi = 0x8F;  // cap at U+10FFFF
        } else {
            return false;
        }
        if (i + length > size) {
            return false;
        }
        if (bytes[i + 1] < first_lo || bytes[i + 1] > first_hi) {
            return false;
        }
        for (std::size_t k = 2; k < length; ++k) {
            if (bytes[i + k] < 0x80 || bytes[i + k] > 0xBF) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

void check_utf8(const std::string& value, const std::string& path, const char* what) {
    if (!is_valid_utf8(value)) {
        tree_error(path, std::string(what) + " is not valid UTF-8");
    }
}

// A String value some constraint promises to accept (a const, an enum value,
// a default) must itself satisfy every other declared constraint — otherwise
// the schema advertises dead options (or an unsatisfiable field) that the
// validator would reject.
void check_string_value_against_constraints(const Field::Spec& spec, const std::string& value, const std::string& path,
                                            const std::string& what) {
    if (spec.non_empty && value.empty()) {
        tree_error(path, what + " violates non_empty()");
    }
    for (const auto& forbidden : spec.forbidden_values) {
        if (forbidden.value == value) {
            tree_error(path, std::format("{} '{}' is a forbidden value", what, value));
        }
    }
    if (spec.pattern && !std::regex_search(value, std::regex(*spec.pattern, std::regex::ECMAScript))) {
        tree_error(path, std::format("{} '{}' does not match pattern", what, value));
    }
}

void check_field(const Field::Spec& spec, const std::string& path, bool in_array_item) {
    check_utf8(spec.key, path, "key");
    check_utf8(spec.title, path, "title");
    check_utf8(spec.description, path, "description");
    check_utf8(spec.placeholder, path, "placeholder");
    if (spec.pattern) {
        check_utf8(*spec.pattern, path, "pattern");
    }
    for (const auto& ev : spec.enum_values) {
        check_utf8(ev.value, path, "enum value");
        check_utf8(ev.title, path, "enum title");
    }
    if (spec.const_value) {
        check_utf8(*spec.const_value, path, "const value");
    }
    for (const auto& forbidden : spec.forbidden_values) {
        check_utf8(forbidden.value, path, "forbidden value");
        check_utf8(forbidden.message, path, "forbidden-value message");
    }
    if (spec.default_string) {
        check_utf8(*spec.default_string, path, "default");
    }

    if (spec.unique && !in_array_item) {
        tree_error(path, "unique() is only meaningful on fields inside an object-array item");
    }
    if (spec.const_value && !spec.enum_values.empty()) {
        tree_error(path, "const_value() and enum_value() are mutually exclusive");
    }
    if (spec.min_int && spec.max_int && *spec.min_int > *spec.max_int) {
        tree_error(path, "min_int exceeds max_int");
    }
    {
        const double lower = spec.min_number ? *spec.min_number
                                             : (spec.exclusive_min_number ? *spec.exclusive_min_number
                                                                          : -std::numeric_limits<double>::infinity());
        const double upper = spec.max_number ? *spec.max_number
                                             : (spec.exclusive_max_number ? *spec.exclusive_max_number
                                                                          : std::numeric_limits<double>::infinity());
        const bool any_exclusive = spec.exclusive_min_number.has_value() || spec.exclusive_max_number.has_value();
        if (lower > upper || (any_exclusive && lower == upper)) {
            tree_error(path, "lower number bound exceeds (or excludes) the upper bound");
        }
    }
    if (spec.pattern) {
        try {
            static_cast<void>(std::regex(*spec.pattern, std::regex::ECMAScript));
        } catch (const std::regex_error& e) {
            tree_error(path, std::string("pattern does not compile: ") + e.what());
        }
    }
    if (spec.const_value) {
        check_string_value_against_constraints(spec, *spec.const_value, path, "const value");
    }
    for (const auto& ev : spec.enum_values) {
        check_string_value_against_constraints(spec, ev.value, path, "enum value");
    }
    if (spec.default_string) {
        const std::string& def = *spec.default_string;
        if (spec.non_empty && def.empty()) {
            tree_error(path, "default violates non_empty()");
        }
        if (spec.const_value && def != *spec.const_value) {
            tree_error(path, "default contradicts const_value()");
        }
        if (!spec.enum_values.empty()) {
            bool found = false;
            for (const auto& ev : spec.enum_values) {
                found = found || ev.value == def;
            }
            if (!found) {
                tree_error(path, "default '" + def + "' is not an enum value");
            }
        }
        for (const auto& forbidden : spec.forbidden_values) {
            if (forbidden.value == def) {
                tree_error(path, "default '" + def + "' is a forbidden value");
            }
        }
        if (spec.pattern && !std::regex_search(def, std::regex(*spec.pattern, std::regex::ECMAScript))) {
            tree_error(path, "default '" + def + "' does not match pattern");
        }
    }
    if (spec.default_int) {
        const std::int64_t min =
            spec.kind == Field::Kind::I2cAddress ? kI2cAddressMin : spec.min_int.value_or(INT64_MIN);
        const std::int64_t max =
            spec.kind == Field::Kind::I2cAddress ? kI2cAddressMax : spec.max_int.value_or(INT64_MAX);
        if (*spec.default_int < min || *spec.default_int > max) {
            tree_error(path, "default " + std::to_string(*spec.default_int) + " is outside the declared bounds");
        }
    }
    if (spec.default_number && ((spec.min_number && *spec.default_number < *spec.min_number) ||
                                (spec.max_number && *spec.default_number > *spec.max_number) ||
                                (spec.exclusive_min_number && *spec.default_number <= *spec.exclusive_min_number) ||
                                (spec.exclusive_max_number && *spec.default_number >= *spec.exclusive_max_number))) {
        tree_error(path, "default " + std::format("{}", *spec.default_number) + " is outside the declared bounds");
    }
}

void check_object(const Object::Spec& spec, const std::string& path, bool in_array_item) {
    check_utf8(spec.title, path, "title");
    check_utf8(spec.description, path, "description");

    std::set<std::string> member_keys;
    std::set<std::string> required_keys;
    // String scalar member key -> its accepted values (enum or const), when it
    // declares any — used to reject unreachable conditional branches.
    std::map<std::string, std::set<std::string>> string_scalar_values;
    std::set<std::string> string_scalar_keys;
    for (const auto& member : spec.members) {
        check_utf8(member.key, path + "." + member.key, "key");
        member_keys.insert(member.key);
        if (member.required) {
            required_keys.insert(member.key);
        }
        const std::string member_path = path + "." + member.key;
        switch (member.kind) {
            case Object::Member::Kind::Scalar:
                // Kind::Scalar implies an engaged field by construction; the
                // has_value() guard exists for the optional-access lint.
                if (member.field.has_value()) {
                    const Field::Spec& field_spec = member.field->spec();
                    if (field_spec.kind == Field::Kind::String) {
                        string_scalar_keys.insert(member.key);
                        std::set<std::string> accepted;
                        for (const auto& ev : field_spec.enum_values) {
                            accepted.insert(ev.value);
                        }
                        const std::optional<std::string>& const_value = field_spec.const_value;
                        if (const_value.has_value()) {
                            accepted.insert(const_value.value());
                        }
                        if (!accepted.empty()) {
                            string_scalar_values[member.key] = std::move(accepted);
                        }
                    }
                    check_field(field_spec, member_path, in_array_item);
                }
                break;
            case Object::Member::Kind::Object:
                check_object(member.object->spec(), member_path, false);
                break;
            case Object::Member::Kind::Array: {
                const Array::Spec& arr = member.array->spec();
                check_utf8(arr.title, member_path, "title");
                check_utf8(arr.description, member_path, "description");
                if (arr.min_items && arr.max_items && *arr.min_items > *arr.max_items) {
                    tree_error(member_path, "min_items exceeds max_items");
                }
                if (arr.item_field) {
                    const Field::Spec& item = arr.item_field->spec();
                    if (item.required) {
                        tree_error(member_path, "required() on an array item spec is meaningless");
                    }
                    if (item.unique) {
                        tree_error(member_path, "use Array::unique() for scalar-array uniqueness");
                    }
                    check_field(item, member_path + "[]", false);
                } else {
                    check_object(arr.item_object->spec(), member_path + "[]", true);
                }
                break;
            }
        }
    }

    std::set<std::pair<std::string, std::string>> seen_branches;
    for (const auto& conditional : spec.conditionals) {
        const std::string cond_path = path + ".when(" + conditional.key + "==" + conditional.equals + ")";
        check_utf8(conditional.equals, cond_path, "equals value");
        if (string_scalar_keys.find(conditional.key) == string_scalar_keys.end()) {
            tree_error(cond_path, "discriminator is not a declared String field");
        }
        // A branch keyed on a value the discriminator can never take would emit
        // an unreachable `if`/`then` — a typo shipping as a silent no-op.
        const auto accepted = string_scalar_values.find(conditional.key);
        if (accepted != string_scalar_values.end() &&
            accepted->second.find(conditional.equals) == accepted->second.end()) {
            tree_error(cond_path, "'" + conditional.equals + "' is not among the discriminator's declared values");
        }
        if (!seen_branches.insert({conditional.key, conditional.equals}).second) {
            tree_error(cond_path, "duplicate branch for this discriminator value");
        }
        std::set<std::string> require_set;
        for (const auto& key : conditional.branch.require_keys()) {
            if (member_keys.find(key) == member_keys.end()) {
                tree_error(cond_path, "require('" + key + "') references an undeclared member");
            }
            require_set.insert(key);
        }
        for (const auto& key : conditional.branch.forbid_keys()) {
            if (member_keys.find(key) == member_keys.end()) {
                tree_error(cond_path, "forbid('" + key + "') references an undeclared member");
            }
            if (required_keys.find(key) != required_keys.end()) {
                tree_error(cond_path, "forbid('" + key + "') contradicts the member being required");
            }
            if (require_set.find(key) != require_set.end()) {
                tree_error(cond_path, "'" + key + "' is both required and forbidden in this branch");
            }
        }
    }

    std::set<std::string> seen_dependents;
    for (const auto& dependent : spec.dependent_required) {
        const std::string dep_path = path + ".dependent_required(" + dependent.key + ")";
        if (member_keys.find(dependent.key) == member_keys.end()) {
            tree_error(dep_path, "references an undeclared member");
        }
        // A repeated key would emit duplicate JSON keys inside
        // `dependentRequired`, and downstream JSON parsers last-win — silently
        // dropping a constraint from the emitted contract.
        if (!seen_dependents.insert(dependent.key).second) {
            tree_error(dep_path, "duplicate dependent_required key");
        }
        std::set<std::string> seen_also;
        for (const auto& key : dependent.also_required) {
            if (member_keys.find(key) == member_keys.end()) {
                tree_error(dep_path, "'" + key + "' references an undeclared member");
            }
            if (key == dependent.key) {
                tree_error(dep_path, "a key cannot depend on itself");
            }
            if (!seen_also.insert(key).second) {
                tree_error(dep_path, "duplicate '" + key + "' in also_required");
            }
        }
    }

    for (const auto& deprecated : spec.deprecated_keys) {
        check_utf8(deprecated.key, path, "deprecated key");
        check_utf8(deprecated.message, path, "deprecated-key message");
        if (member_keys.find(deprecated.key) != member_keys.end()) {
            tree_error(path + "." + deprecated.key, "deprecated_key collides with a declared member");
        }
    }
}

// ---- JSON Schema emission ------------------------------------------------

JObject emit_object(const Object::Spec& spec, const std::string* title_override,
                    const std::string* description_override);

JObject emit_field(const Field::Spec& spec) {
    JObject out;
    const auto add_common_prefix = [&out, &spec]() {
        if (!spec.title.empty()) {
            out.emplace_back("title", spec.title);
        }
        if (!spec.description.empty()) {
            out.emplace_back("description", spec.description);
        }
    };
    const auto add_common_suffix = [&out, &spec]() {
        if (!spec.placeholder.empty()) {
            out.emplace_back("x-anolis-placeholder", spec.placeholder);
        }
        if (spec.unique) {
            out.emplace_back("x-anolis-unique", true);
        }
    };

    switch (spec.kind) {
        case Field::Kind::String: {
            out.emplace_back("type", "string");
            add_common_prefix();
            if (spec.const_value) {
                out.emplace_back("const", *spec.const_value);
            }
            if (!spec.enum_values.empty()) {
                bool any_title = false;
                for (const auto& ev : spec.enum_values) {
                    any_title = any_title || !ev.title.empty();
                }
                if (any_title) {
                    JArray one_of;
                    for (const auto& ev : spec.enum_values) {
                        JObject entry;
                        entry.emplace_back("const", ev.value);
                        if (!ev.title.empty()) {
                            entry.emplace_back("title", ev.title);
                        }
                        one_of.emplace_back(std::move(entry));
                    }
                    out.emplace_back("oneOf", std::move(one_of));
                } else {
                    JArray values;
                    for (const auto& ev : spec.enum_values) {
                        values.emplace_back(ev.value);
                    }
                    out.emplace_back("enum", std::move(values));
                }
            }
            if (spec.pattern) {
                out.emplace_back("pattern", *spec.pattern);
            }
            if (spec.non_empty) {
                out.emplace_back("minLength", 1);
            }
            if (!spec.forbidden_values.empty()) {
                JObject not_schema;
                if (spec.forbidden_values.size() == 1) {
                    not_schema.emplace_back("const", spec.forbidden_values.front().value);
                } else {
                    JArray values;
                    for (const auto& forbidden : spec.forbidden_values) {
                        values.emplace_back(forbidden.value);
                    }
                    not_schema.emplace_back("enum", std::move(values));
                }
                out.emplace_back("not", std::move(not_schema));
            }
            if (spec.default_string) {
                out.emplace_back("default", *spec.default_string);
            }
            break;
        }
        case Field::Kind::Integer: {
            out.emplace_back("type", "integer");
            add_common_prefix();
            if (spec.min_int) {
                out.emplace_back("minimum", *spec.min_int);
            }
            if (spec.max_int) {
                out.emplace_back("maximum", *spec.max_int);
            }
            if (spec.default_int) {
                out.emplace_back("default", *spec.default_int);
            }
            break;
        }
        case Field::Kind::Number: {
            out.emplace_back("type", "number");
            add_common_prefix();
            if (spec.min_number) {
                out.emplace_back("minimum", *spec.min_number);
            }
            if (spec.exclusive_min_number) {
                out.emplace_back("exclusiveMinimum", *spec.exclusive_min_number);
            }
            if (spec.max_number) {
                out.emplace_back("maximum", *spec.max_number);
            }
            if (spec.exclusive_max_number) {
                out.emplace_back("exclusiveMaximum", *spec.exclusive_max_number);
            }
            if (spec.default_number) {
                out.emplace_back("default", *spec.default_number);
            }
            break;
        }
        case Field::Kind::Boolean: {
            out.emplace_back("type", "boolean");
            add_common_prefix();
            if (spec.default_bool) {
                out.emplace_back("default", *spec.default_bool);
            }
            break;
        }
        case Field::Kind::I2cAddress: {
            add_common_prefix();
            JObject int_branch;
            int_branch.emplace_back("type", "integer");
            int_branch.emplace_back("minimum", kI2cAddressMin);
            int_branch.emplace_back("maximum", kI2cAddressMax);
            JObject string_branch;
            string_branch.emplace_back("type", "string");
            string_branch.emplace_back("pattern", kI2cHexPattern);
            JArray any_of;
            any_of.emplace_back(std::move(int_branch));
            any_of.emplace_back(std::move(string_branch));
            out.emplace_back("anyOf", std::move(any_of));
            if (spec.default_int) {
                out.emplace_back("default", *spec.default_int);
            }
            out.emplace_back("x-anolis-type", "i2c_address");
            break;
        }
    }
    add_common_suffix();
    return out;
}

JObject emit_array(const Array::Spec& spec) {
    JObject out;
    out.emplace_back("type", "array");
    if (!spec.title.empty()) {
        out.emplace_back("title", spec.title);
    }
    if (!spec.description.empty()) {
        out.emplace_back("description", spec.description);
    }
    if (spec.item_field) {
        out.emplace_back("items", emit_field(spec.item_field->spec()));
    } else {
        out.emplace_back("items", emit_object(spec.item_object->spec(), nullptr, nullptr));
    }
    if (spec.min_items) {
        out.emplace_back("minItems", static_cast<std::int64_t>(*spec.min_items));
    }
    if (spec.max_items) {
        out.emplace_back("maxItems", static_cast<std::int64_t>(*spec.max_items));
    }
    if (spec.unique) {
        // `uniqueItems` is best-effort for standard validators: it compares
        // JSON values, so 0x08 and "0x08" would NOT collide under it. The SDK
        // validator compares parsed values; x-anolis-unique carries that intent.
        out.emplace_back("uniqueItems", true);
        out.emplace_back("x-anolis-unique", true);
    }
    return out;
}

JObject emit_object(const Object::Spec& spec, const std::string* title_override,
                    const std::string* description_override) {
    JObject out;
    out.emplace_back("type", "object");
    const std::string& title = title_override != nullptr ? *title_override : spec.title;
    const std::string& description = description_override != nullptr ? *description_override : spec.description;
    if (!title.empty()) {
        out.emplace_back("title", title);
    }
    if (!description.empty()) {
        out.emplace_back("description", description);
    }

    JObject properties;
    JArray required;
    for (const auto& member : spec.members) {
        switch (member.kind) {
            case Object::Member::Kind::Scalar:
                if (member.field.has_value()) {  // always true for Kind::Scalar; lint guard
                    properties.emplace_back(member.key, emit_field(member.field->spec()));
                }
                break;
            case Object::Member::Kind::Object:
                properties.emplace_back(member.key, emit_object(member.object->spec(), nullptr, nullptr));
                break;
            case Object::Member::Kind::Array:
                properties.emplace_back(member.key, emit_array(member.array->spec()));
                break;
        }
        if (member.required) {
            required.emplace_back(member.key);
        }
    }
    if (!properties.empty()) {
        out.emplace_back("properties", std::move(properties));
    }
    if (!required.empty()) {
        out.emplace_back("required", std::move(required));
    }
    if (spec.openness == Openness::Closed) {
        out.emplace_back("additionalProperties", false);
    }

    if (!spec.dependent_required.empty()) {
        JObject dependent;
        for (const auto& entry : spec.dependent_required) {
            JArray keys;
            for (const auto& key : entry.also_required) {
                keys.emplace_back(key);
            }
            dependent.emplace_back(entry.key, std::move(keys));
        }
        out.emplace_back("dependentRequired", std::move(dependent));
    }

    if (!spec.conditionals.empty()) {
        JArray all_of;
        for (const auto& conditional : spec.conditionals) {
            // `required` inside the `if` is load-bearing: `properties` alone is
            // vacuously true when the discriminator is absent, which would fire
            // every branch's `then` at once.
            JObject discriminator;
            discriminator.emplace_back("const", conditional.equals);
            JObject if_properties;
            if_properties.emplace_back(conditional.key, std::move(discriminator));
            JObject if_schema;
            if_schema.emplace_back("properties", std::move(if_properties));
            if_schema.emplace_back("required", JArray{JsonValue(conditional.key)});

            JObject then_schema;
            if (!conditional.branch.require_keys().empty()) {
                JArray keys;
                for (const auto& key : conditional.branch.require_keys()) {
                    keys.emplace_back(key);
                }
                then_schema.emplace_back("required", std::move(keys));
            }
            if (!conditional.branch.forbid_keys().empty()) {
                JObject forbidden;
                for (const auto& key : conditional.branch.forbid_keys()) {
                    forbidden.emplace_back(key, false);  // `false` schema: key may not appear
                }
                then_schema.emplace_back("properties", std::move(forbidden));
            }

            JObject branch;
            branch.emplace_back("if", std::move(if_schema));
            branch.emplace_back("then", std::move(then_schema));
            all_of.emplace_back(std::move(branch));
        }
        out.emplace_back("allOf", std::move(all_of));
    }
    return out;
}

JObject emit_schema(const Schema& schema) {
    JObject out;
    out.emplace_back("$schema", "https://json-schema.org/draft/2020-12/schema");
    const std::optional<std::string>& id = schema.schema_id();
    if (id.has_value()) {
        out.emplace_back("$id", id.value());
    }
    const std::optional<std::string>& title_opt = schema.schema_title();
    const std::optional<std::string>& description_opt = schema.schema_description();
    const std::string* title = title_opt.has_value() ? &title_opt.value() : nullptr;
    const std::string* description = description_opt.has_value() ? &description_opt.value() : nullptr;
    JObject body = emit_object(schema.root().spec(), title, description);
    for (auto& entry : body) {
        out.emplace_back(std::move(entry));
    }
    return out;
}

}  // namespace

// ---- Field ---------------------------------------------------------------

namespace {

[[noreturn]] void field_error(const std::string& key, const std::string& message) {
    throw std::logic_error("config schema field '" + key + "': " + message);
}

}  // namespace

Field::Field(std::string key, Kind kind) {
    if (key.empty()) {
        throw std::logic_error("config schema field: key must not be empty");
    }
    spec_.key = std::move(key);
    spec_.kind = kind;
}

Field string_field(std::string key) { return Field(std::move(key), Field::Kind::String); }
Field integer_field(std::string key) { return Field(std::move(key), Field::Kind::Integer); }
Field number_field(std::string key) { return Field(std::move(key), Field::Kind::Number); }
Field boolean_field(std::string key) { return Field(std::move(key), Field::Kind::Boolean); }
Field i2c_address_field(std::string key) { return Field(std::move(key), Field::Kind::I2cAddress); }

Field& Field::required() {
    spec_.required = true;
    return *this;
}

Field& Field::non_empty() {
    if (spec_.kind != Kind::String) {
        field_error(spec_.key, "non_empty() applies to String fields only");
    }
    spec_.non_empty = true;
    return *this;
}

Field& Field::unique() {
    spec_.unique = true;
    return *this;
}

Field& Field::pattern(std::string anchored_ecma_regex) {
    if (spec_.kind != Kind::String) {
        field_error(spec_.key, "pattern() applies to String fields only");
    }
    spec_.pattern = std::move(anchored_ecma_regex);
    return *this;
}

Field& Field::enum_value(std::string value) { return enum_value(std::move(value), ""); }

Field& Field::enum_value(std::string value, std::string title) {
    if (spec_.kind != Kind::String) {
        field_error(spec_.key, "enum_value() applies to String fields only");
    }
    for (const auto& existing : spec_.enum_values) {
        if (existing.value == value) {
            field_error(spec_.key, "duplicate enum value '" + value + "'");
        }
    }
    spec_.enum_values.push_back({std::move(value), std::move(title)});
    return *this;
}

Field& Field::const_value(std::string value) {
    if (spec_.kind != Kind::String) {
        field_error(spec_.key, "const_value() applies to String fields only");
    }
    spec_.const_value = std::move(value);
    return *this;
}

Field& Field::forbid_value(std::string value, std::string message) {
    if (spec_.kind != Kind::String) {
        field_error(spec_.key, "forbid_value() applies to String fields only");
    }
    for (const auto& existing : spec_.forbidden_values) {
        if (existing.value == value) {
            field_error(spec_.key, "duplicate forbidden value '" + value + "'");
        }
    }
    spec_.forbidden_values.push_back({std::move(value), std::move(message)});
    return *this;
}

Field& Field::min_int(std::int64_t minimum) {
    if (spec_.kind != Kind::Integer) {
        field_error(spec_.key, "min_int() applies to Integer fields only (I2cAddress bounds are built in)");
    }
    spec_.min_int = minimum;
    return *this;
}

Field& Field::max_int(std::int64_t maximum) {
    if (spec_.kind != Kind::Integer) {
        field_error(spec_.key, "max_int() applies to Integer fields only (I2cAddress bounds are built in)");
    }
    spec_.max_int = maximum;
    return *this;
}

Field& Field::min_number(double minimum) {
    if (spec_.kind != Kind::Number) {
        field_error(spec_.key, "min_number() applies to Number fields only");
    }
    if (!std::isfinite(minimum)) {
        field_error(spec_.key, "min_number() must be finite");
    }
    if (spec_.exclusive_min_number) {
        field_error(spec_.key, "min_number() conflicts with exclusive_min_number()");
    }
    spec_.min_number = minimum;
    return *this;
}

Field& Field::max_number(double maximum) {
    if (spec_.kind != Kind::Number) {
        field_error(spec_.key, "max_number() applies to Number fields only");
    }
    if (!std::isfinite(maximum)) {
        field_error(spec_.key, "max_number() must be finite");
    }
    if (spec_.exclusive_max_number) {
        field_error(spec_.key, "max_number() conflicts with exclusive_max_number()");
    }
    spec_.max_number = maximum;
    return *this;
}

Field& Field::exclusive_min_number(double minimum) {
    if (spec_.kind != Kind::Number) {
        field_error(spec_.key, "exclusive_min_number() applies to Number fields only");
    }
    if (!std::isfinite(minimum)) {
        field_error(spec_.key, "exclusive_min_number() must be finite");
    }
    if (spec_.min_number) {
        field_error(spec_.key, "exclusive_min_number() conflicts with min_number()");
    }
    spec_.exclusive_min_number = minimum;
    return *this;
}

Field& Field::exclusive_max_number(double maximum) {
    if (spec_.kind != Kind::Number) {
        field_error(spec_.key, "exclusive_max_number() applies to Number fields only");
    }
    if (!std::isfinite(maximum)) {
        field_error(spec_.key, "exclusive_max_number() must be finite");
    }
    if (spec_.max_number) {
        field_error(spec_.key, "exclusive_max_number() conflicts with max_number()");
    }
    spec_.exclusive_max_number = maximum;
    return *this;
}

Field& Field::default_string(std::string value) {
    if (spec_.kind != Kind::String) {
        field_error(spec_.key, "default_string() applies to String fields only");
    }
    spec_.default_string = std::move(value);
    return *this;
}

Field& Field::default_int(std::int64_t value) {
    if (spec_.kind != Kind::Integer && spec_.kind != Kind::I2cAddress) {
        field_error(spec_.key, "default_int() applies to Integer/I2cAddress fields only");
    }
    spec_.default_int = value;
    return *this;
}

Field& Field::default_number(double value) {
    if (spec_.kind != Kind::Number) {
        field_error(spec_.key, "default_number() applies to Number fields only");
    }
    if (!std::isfinite(value)) {
        field_error(spec_.key, "default_number() must be finite");
    }
    spec_.default_number = value;
    return *this;
}

Field& Field::default_bool(bool value) {
    if (spec_.kind != Kind::Boolean) {
        field_error(spec_.key, "default_bool() applies to Boolean fields only");
    }
    spec_.default_bool = value;
    return *this;
}

Field& Field::title(std::string text) {
    spec_.title = std::move(text);
    return *this;
}

Field& Field::description(std::string text) {
    spec_.description = std::move(text);
    return *this;
}

Field& Field::placeholder(std::string text) {
    spec_.placeholder = std::move(text);
    return *this;
}

// ---- Array ---------------------------------------------------------------

Array Array::of_scalars(Field item) {
    Array out;
    out.spec_.item_field = std::move(item);
    return out;
}

Array Array::of_objects(Object item) {
    Array out;
    out.spec_.item_object = std::make_shared<const Object>(std::move(item));
    return out;
}

Array& Array::min_items(std::size_t count) {
    spec_.min_items = count;
    return *this;
}

Array& Array::max_items(std::size_t count) {
    spec_.max_items = count;
    return *this;
}

Array& Array::unique() {
    if (spec_.item_object) {
        throw std::logic_error(
            "config schema: Array::unique() applies to scalar arrays only; "
            "mark the distinguishing item field with Field::unique() instead");
    }
    spec_.unique = true;
    return *this;
}

Array& Array::title(std::string text) {
    spec_.title = std::move(text);
    return *this;
}

Array& Array::description(std::string text) {
    spec_.description = std::move(text);
    return *this;
}

// ---- When / Object -------------------------------------------------------

When& When::require(std::string key) {
    require_keys_.push_back(std::move(key));
    return *this;
}

When& When::forbid(std::string key) {
    forbid_keys_.push_back(std::move(key));
    return *this;
}

Object::Object(Openness openness) { spec_.openness = openness; }

void Object::insert_member(Member member) {
    for (const auto& existing : spec_.members) {
        if (existing.key == member.key) {
            throw std::logic_error("config schema: duplicate member key '" + member.key + "'");
        }
    }
    spec_.members.push_back(std::move(member));
}

Object& Object::field(Field f) {
    Member member;
    member.key = f.spec().key;
    member.required = f.spec().required;
    member.kind = Member::Kind::Scalar;
    member.field = std::move(f);
    insert_member(std::move(member));
    return *this;
}

Object& Object::child(std::string key, Object obj, Presence presence) {
    Member member;
    member.key = std::move(key);
    member.required = presence == Presence::Required;
    member.kind = Member::Kind::Object;
    member.object = std::make_shared<const Object>(std::move(obj));
    insert_member(std::move(member));
    return *this;
}

Object& Object::array(std::string key, Array arr, Presence presence) {
    Member member;
    member.key = std::move(key);
    member.required = presence == Presence::Required;
    member.kind = Member::Kind::Array;
    member.array = std::make_shared<const Array>(std::move(arr));
    insert_member(std::move(member));
    return *this;
}

Object& Object::when(std::string key, std::string equals, When branch) {
    if (branch.require_keys().empty() && branch.forbid_keys().empty()) {
        throw std::logic_error("config schema: when(" + key + "==" + equals +
                               "): branch requires and forbids nothing (vacuous)");
    }
    spec_.conditionals.push_back({std::move(key), std::move(equals), std::move(branch)});
    return *this;
}

Object& Object::dependent_required(std::string key, std::vector<std::string> also_required) {
    spec_.dependent_required.push_back({std::move(key), std::move(also_required)});
    return *this;
}

Object& Object::deprecated_key(std::string key, std::string message) {
    spec_.deprecated_keys.push_back({std::move(key), std::move(message)});
    return *this;
}

Object& Object::title(std::string text) {
    spec_.title = std::move(text);
    return *this;
}

Object& Object::description(std::string text) {
    spec_.description = std::move(text);
    return *this;
}

// ---- Schema / emission entry points --------------------------------------

Schema::Schema(Object root) : root_(std::move(root)) { check_object(root_.spec(), "root", false); }

Schema& Schema::id(std::string value) {
    if (!is_valid_utf8(value)) {
        throw std::logic_error("config schema: $id is not valid UTF-8");
    }
    id_ = std::move(value);
    return *this;
}

Schema& Schema::title(std::string text) {
    if (!is_valid_utf8(text)) {
        throw std::logic_error("config schema: title is not valid UTF-8");
    }
    title_ = std::move(text);
    return *this;
}

Schema& Schema::description(std::string text) {
    if (!is_valid_utf8(text)) {
        throw std::logic_error("config schema: description is not valid UTF-8");
    }
    description_ = std::move(text);
    return *this;
}

std::string to_json_schema(const Schema& schema) { return serialize(JsonValue(emit_schema(schema))); }

std::string config_schema_envelope(const Schema& schema, const EnvelopeOptions& options) {
    JObject envelope;
    envelope.emplace_back("config_schema_version", kConfigSchemaVersion);
    if (!options.provider_name.empty()) {
        if (!is_valid_utf8(options.provider_name)) {
            throw std::logic_error("config schema envelope: provider_name is not valid UTF-8");
        }
        envelope.emplace_back("provider", options.provider_name);
    }
    for (const auto& [key, value] : options.extra_string_entries) {
        if (!is_valid_utf8(key) || !is_valid_utf8(value)) {
            throw std::logic_error("config schema envelope: extra entry '" + key + "' is not valid UTF-8");
        }
        if (key == "config_schema_version" || key == "schema" || key == "provider") {
            throw std::logic_error("config schema envelope: extra entry '" + key + "' collides with a reserved key");
        }
        for (const auto& existing : envelope) {
            if (existing.first == key) {
                throw std::logic_error("config schema envelope: duplicate extra entry '" + key + "'");
            }
        }
        envelope.emplace_back(key, value);
    }
    envelope.emplace_back("schema", emit_schema(schema));
    return serialize(JsonValue(std::move(envelope)));
}

void write_config_schema_envelope(std::ostream& out, const Schema& schema, const EnvelopeOptions& options) {
    out << config_schema_envelope(schema, options) << '\n';
}

}  // namespace anolis::provider_sdk::config
