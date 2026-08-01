#pragma once

// Config-schema toolkit, part 2 (declare-once, sdk#24): validate a parsed YAML
// config against the SAME `Schema` declaration that `--config-schema` emits —
// the schema a client reads and the validation `--check-config` enforces
// cannot drift, because both are derived from one declaration.
//
// Scalar typing follows the YAML 1.2 core schema so the emitted JSON Schema is
// honest about what the validator accepts:
//   - a QUOTED scalar (or `!!str`) is a string — `timeout_ms: "300"` is a type
//     error against `type: integer`;
//   - a PLAIN scalar resolves by the core-schema rules: `true`/`false` are
//     booleans, `300`, `0x63`, `0o17` are integers (`010` is DECIMAL ten, not
//     octal), `0.5`/`5e-3`/`.inf`/`.nan` are floats, everything else a string;
//   - per JSON Schema semantics, `type: integer` also accepts a float with a
//     zero fractional part (`5.0`), but never `5.7` — and trailing junk
//     (`5abc`) is a string, never five.
//
// The same resolver backs the typed extraction helpers below, so a provider's
// post-validation config *parsing* uses identical typing rules — closing the
// third drift surface (validate one way, extract another).
//
// Uniqueness (`Field::unique()` / `Array::unique()`) compares PARSED values:
// `0x08` and `"0x08"` collide on an I2C-address field even though they are
// different JSON values.

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "anolis/provider_sdk/config.hpp"

namespace anolis::provider_sdk::config {

// One validation failure. `path` is dotted with [i] indices ("devices[2].id");
// "" denotes the document root.
struct ValidationError {
    std::string path;
    std::string message;
};

// Validate `root` (a parsed YAML document) against the declaration. Collects
// ALL errors rather than failing on the first — `--check-config` reports the
// whole story, and a form renderer can mark every offending field.
std::vector<ValidationError> validate(const Schema& schema, const YAML::Node& root);

// "path: message" per line — for providers that surface a single throw/log.
std::string format_errors(const std::vector<ValidationError>& errors);

// ---- typed extraction (post-validation parsing) ---------------------------
//
// Each returns nullopt when the node is absent, non-scalar, or does not
// resolve to the requested type under the SAME rules `validate()` enforces.
// After a passing `validate()`, extraction of a declared field cannot fail —
// use these instead of yaml-cpp's `as<T>()` (whose coercions accept what the
// validator rejects, e.g. `as<double>("5")`). One deliberate leniency:
// `as_double` returns `.inf`/`.nan` (they ARE floats under the core schema)
// even though `validate()` rejects them for Number fields — irrelevant after
// a passing validate, but visible to raw-node callers.

// The YAML 1.2 core-schema resolution of a node.
enum class ScalarKind {
    Null,
    Bool,
    Int,
    Float,
    String,
    NonScalar,  // maps, sequences, and undefined nodes
};

ScalarKind resolved_kind(const YAML::Node& node);

std::optional<std::string> as_string(const YAML::Node& node);
// Ints, plus floats with zero fractional part (JSON Schema integer semantics).
std::optional<std::int64_t> as_int64(const YAML::Node& node);
std::optional<double> as_double(const YAML::Node& node);
std::optional<bool> as_bool(const YAML::Node& node);

// An I2C address per the built-in scalar: a resolved integer, or a string of
// the form `0xNN`; nullopt when neither or outside 7-bit 0x08-0x77.
std::optional<int> parse_i2c_address(const YAML::Node& node);

}  // namespace anolis::provider_sdk::config
