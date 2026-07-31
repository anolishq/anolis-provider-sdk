#pragma once

// Config-schema toolkit (declare-once, sdk#24): a provider declares its config
// contract ONCE as a `Schema`, and the SDK derives every downstream surface
// from that single declaration so they cannot drift:
//
//   1. `--config-schema` — `write_config_schema_envelope()` emits the versioned
//      JSON envelope defined by the Anolis executable profile v1 §2
//      (anolis-protocol `docs/profiles/anolis-executable-profile-v1.md`):
//      `{"config_schema_version": 1, "provider": ..., "schema": {<JSON Schema>}}`.
//   2. `--check-config` — the YAML validator (part 2 of the lift) walks a
//      parsed config against the same declaration.
//
// The emitted schema targets JSON Schema draft 2020-12. Where JSON Schema has
// no native slot for a constraint the SDK enforces, the schema carries an
// `x-anolis-*` annotation so consumers (e.g. the workbench form renderer) keep
// the information:
//   - `x-anolis-unique: true` — the field's parsed value must be unique across
//     the enclosing array's items (JSON Schema `uniqueItems` compares JSON
//     values, so it cannot see that `0x08` and `"0x08"` collide).
//   - `x-anolis-placeholder` — form placeholder text.
//   - `x-anolis-type: "i2c_address"` — the built-in I2C address scalar: an
//     integer or a `0x`-prefixed hex string, constrained to the 7-bit
//     addressable range 0x08–0x77.
//
// Builder misuse (kind-mismatched setters, duplicate keys, contradictory or
// non-finite constraints, non-UTF-8 text) throws `std::logic_error` eagerly —
// a provider's schema is static code, so declaration bugs should fail its unit
// tests, not ship. Patterns are anchored ECMAScript regexes; JSON Schema
// `pattern` uses *search* semantics, so an unanchored pattern would silently
// accept substrings downstream. Author patterns anchored (`^...$`).
//
// Boundary: conditionals see SIBLING keys only. A cross-scope rule (e.g. "this
// array-item key is only valid when a root-level mode field says so") is not
// declarable; it stays a provider-side semantic check, and the emitted schema
// is honestly *looser* there. Likewise `deprecated_key` is validator-side only:
// on a Closed object external validators still reject the key generically via
// `additionalProperties: false`, but on an Open object the emitted schema
// accepts what `--check-config` will reject — prefer Closed objects for
// sections with removed keys.
//
// The fluent API is safe when chained into a sink within one full expression
// (`obj.field(string_field("x").required())`); do NOT bind a chain to a
// reference (`auto& f = string_field("x").required();` dangles).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace anolis::provider_sdk::config {

// The envelope-convention version this SDK emits (executable profile v1 §2).
// Versions the ENVELOPE shape, not any provider's schema.
inline constexpr int kConfigSchemaVersion = 1;

// Unknown-key policy for an object. Explicit — no default — because the fleet
// genuinely differs per object (sim's root is open, its `simulation` section is
// closed) and a silent default would tighten or loosen a provider by accident.
enum class Openness {
    Closed,  // reject unknown keys (JSON Schema `additionalProperties: false`)
    Open,    // allow unknown keys (additionalProperties omitted)
};

// Whether a non-scalar member (child object / array) must be present.
enum class Presence {
    Optional,
    Required,
};

// A scalar field declaration. Construct via the factory free functions
// (`string_field`, `integer_field`, ...) and chain setters; setters validate
// against the field kind and throw `std::logic_error` on misuse.
class Field {
public:
    enum class Kind { String, Integer, Number, Boolean, I2cAddress };

    struct EnumValue {
        std::string value;
        std::string title;  // optional display title (workbench form labels)
    };

    struct ForbiddenValue {
        std::string value;
        std::string message;  // validator-side error message ("" = generic)
    };

    struct Spec {
        std::string key;
        Kind kind = Kind::String;
        bool required = false;
        bool non_empty = false;  // String: minLength 1
        bool unique = false;     // unique parsed value across array items
        std::optional<std::string> pattern;
        std::vector<EnumValue> enum_values;
        std::optional<std::string> const_value;
        std::vector<ForbiddenValue> forbidden_values;
        std::optional<std::int64_t> min_int;
        std::optional<std::int64_t> max_int;
        std::optional<double> min_number;
        std::optional<double> max_number;
        std::optional<double> exclusive_min_number;
        std::optional<double> exclusive_max_number;
        std::optional<std::string> default_string;
        std::optional<std::int64_t> default_int;
        std::optional<double> default_number;
        std::optional<bool> default_bool;
        std::string title;
        std::string description;
        std::string placeholder;
    };

    Field& required();

    // String: require a non-empty value (minLength 1).
    Field& non_empty();

    // Mark this field's parsed value unique across the items of the enclosing
    // array (e.g. `devices[].id`). Only meaningful inside an object-array item;
    // `Schema` construction rejects it anywhere else.
    Field& unique();

    // String: anchored ECMAScript regex the value must match.
    Field& pattern(std::string anchored_ecma_regex);

    // String: append an allowed value (with an optional display title). A field
    // with enum values emits `enum: [...]`, or `oneOf: [{const, title}, ...]`
    // when any value carries a title.
    Field& enum_value(std::string value);
    Field& enum_value(std::string value, std::string title);

    // String: the only accepted value (JSON Schema `const`).
    Field& const_value(std::string value);

    // String: a value that is specifically rejected (JSON Schema `not`), with an
    // optional validator-side message (e.g. a reserved device id).
    Field& forbid_value(std::string value, std::string message = "");

    // Integer bounds (inclusive). I2cAddress carries fixed built-in bounds.
    Field& min_int(std::int64_t minimum);
    Field& max_int(std::int64_t maximum);

    // Number bounds (inclusive). Non-finite bounds throw.
    Field& min_number(double minimum);
    Field& max_number(double maximum);

    // Number bounds (exclusive; JSON Schema `exclusiveMinimum`/`Maximum`).
    // Mutually exclusive with the inclusive form on the same side.
    Field& exclusive_min_number(double minimum);
    Field& exclusive_max_number(double maximum);

    // Typed defaults — documentation/form hints ONLY. The validator never
    // materializes defaults into a config; dynamic defaults (e.g. "label
    // defaults to the id") cannot be declared and belong in `description`.
    Field& default_string(std::string value);
    Field& default_int(std::int64_t value);
    Field& default_number(double value);
    Field& default_bool(bool value);

    Field& title(std::string text);
    Field& description(std::string text);
    Field& placeholder(std::string text);  // emitted as x-anolis-placeholder

    const Spec& spec() const { return spec_; }

private:
    Field(std::string key, Kind kind);

    Spec spec_;

    friend Field string_field(std::string key);
    friend Field integer_field(std::string key);
    friend Field number_field(std::string key);
    friend Field boolean_field(std::string key);
    friend Field i2c_address_field(std::string key);
};

// Scalar field factories.
Field string_field(std::string key);
Field integer_field(std::string key);
Field number_field(std::string key);
Field boolean_field(std::string key);
// Built-in I2C address scalar: integer or `0x`-prefixed hex string, 7-bit
// addressable range 0x08–0x77 (both emitted branches carry the range).
Field i2c_address_field(std::string key);

class Object;

// An array member declaration: either scalar items or object items.
class Array {
public:
    struct Spec {
        std::optional<Field> item_field;            // engaged for scalar items
        std::shared_ptr<const Object> item_object;  // engaged for object items
        std::optional<std::size_t> min_items;
        std::optional<std::size_t> max_items;
        bool unique = false;  // scalar arrays: parsed-value uniqueness
        std::string title;
        std::string description;
    };

    static Array of_scalars(Field item);
    static Array of_objects(Object item);

    Array& min_items(std::size_t count);
    Array& max_items(std::size_t count);

    // Scalar arrays: items' parsed values must be pairwise distinct (emits
    // `uniqueItems: true` + `x-anolis-unique: true`; the SDK validator compares
    // PARSED values, e.g. `0x08` vs `"0x08"` collide). Object arrays throw —
    // mark the distinguishing field with `Field::unique()` instead.
    Array& unique();

    Array& title(std::string text);
    Array& description(std::string text);

    const Spec& spec() const { return spec_; }

private:
    Array() = default;

    Spec spec_;
};

// A conditional branch used with `Object::when()`: when the discriminator field
// equals a value, additional keys become required and/or forbidden. Emitted as
// `allOf: [{if: {properties: {K: {const: V}}, required: [K]}, then: ...}]` —
// the `required` in the `if` is load-bearing: without it every branch fires
// vacuously when the discriminator is absent.
class When {
public:
    When& require(std::string key);
    When& forbid(std::string key);

    const std::vector<std::string>& require_keys() const { return require_keys_; }
    const std::vector<std::string>& forbid_keys() const { return forbid_keys_; }

private:
    std::vector<std::string> require_keys_;
    std::vector<std::string> forbid_keys_;
};

// An object declaration: named members (scalar fields, child objects, arrays),
// an explicit unknown-key policy, and object-level constraints. Member keys are
// unique; inserting a duplicate throws.
class Object {
public:
    struct Member {
        enum class Kind { Scalar, Object, Array };

        std::string key;
        bool required = false;
        Kind kind = Kind::Scalar;
        std::optional<Field> field;            // Kind::Scalar
        std::shared_ptr<const Object> object;  // Kind::Object
        std::shared_ptr<const Array> array;    // Kind::Array
    };

    struct Conditional {
        std::string key;     // discriminator member (a declared String scalar)
        std::string equals;  // the value selecting this branch
        When branch;
    };

    struct DeprecatedKey {
        std::string key;
        std::string message;  // validator-side error message
    };

    struct DependentRequired {
        std::string key;
        std::vector<std::string> also_required;
    };

    struct Spec {
        Openness openness = Openness::Closed;
        std::vector<Member> members;
        std::vector<Conditional> conditionals;
        std::vector<DependentRequired> dependent_required;
        std::vector<DeprecatedKey> deprecated_keys;
        std::string title;
        std::string description;
    };

    explicit Object(Openness openness);

    // Scalar member; presence comes from `Field::required()`.
    Object& field(Field f);

    // Nested object member.
    Object& child(std::string key, Object obj, Presence presence = Presence::Optional);

    // Array member.
    Object& array(std::string key, Array arr, Presence presence = Presence::Optional);

    // Conditional requirements keyed on a declared String member's value
    // (e.g. `when("mode", "manual", When().require("addresses"))`).
    Object& when(std::string key, std::string equals, When branch);

    // JSON Schema `dependentRequired`: when `key` is present, `also_required`
    // must be too (e.g. sim's ambient_signal_path -> ambient_temp_c).
    Object& dependent_required(std::string key, std::vector<std::string> also_required);

    // A key that is rejected with a specific message (e.g. a removed setting).
    // Validator-side only; nothing is emitted. Must not collide with a member.
    Object& deprecated_key(std::string key, std::string message);

    Object& title(std::string text);
    Object& description(std::string text);

    const Spec& spec() const { return spec_; }

private:
    void insert_member(Member member);

    Spec spec_;
};

// A provider's full config schema: the validated root object plus optional
// root-level metadata. Construction deep-checks the declaration tree
// (conditionals/dependents reference declared keys, forbid targets are not
// required, defaults satisfy their own constraints, `unique` appears only
// inside array items, ...) and throws `std::logic_error` with the offending
// path on any inconsistency.
class Schema {
public:
    explicit Schema(Object root);

    // Optional provider-owned schema identity (profile §2 leaves config-schema
    // versioning to the provider; `$id` is the conventional slot).
    Schema& id(std::string value);
    Schema& title(std::string text);
    Schema& description(std::string text);

    const Object& root() const { return root_; }
    const std::optional<std::string>& schema_id() const { return id_; }
    const std::optional<std::string>& schema_title() const { return title_; }
    const std::optional<std::string>& schema_description() const { return description_; }

private:
    Object root_;
    std::optional<std::string> id_;
    std::optional<std::string> title_;
    std::optional<std::string> description_;
};

// Envelope metadata (executable profile v1 §2). `provider_name` is the
// recommended `provider` key — pass the `provider_name` advertised in Hello /
// `--provider-profile`; empty omits the key. `extra_string_entries` become
// additional top-level envelope keys (provider-specific extras are allowed by
// the profile); they may not collide with the reserved envelope keys.
struct EnvelopeOptions {
    std::string provider_name;
    std::vector<std::pair<std::string, std::string>> extra_string_entries;
};

// The bare JSON Schema document (draft 2020-12), pretty-printed, no trailing
// newline. Key order is deterministic (declaration order) so output is stable
// across runs and diffable across provider versions.
std::string to_json_schema(const Schema& schema);

// The full `--config-schema` envelope document, pretty-printed.
std::string config_schema_envelope(const Schema& schema, const EnvelopeOptions& options);

// Writes the envelope plus a trailing newline — everything a provider's
// `--config-schema` verb should print to stdout (diagnostics go to stderr;
// the verb must run before anything else can touch stdout).
void write_config_schema_envelope(std::ostream& out, const Schema& schema, const EnvelopeOptions& options);

}  // namespace anolis::provider_sdk::config
