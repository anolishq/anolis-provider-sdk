// Config-schema toolkit part 2 (declare-once, sdk#24) — the YAML validator +
// typed extraction helpers, driven by the same declaration the emitter uses.
//
// The typing seam is the load-bearing correctness surface here: scalars
// resolve by the YAML 1.2 core schema (quoted "300" is a STRING; plain 0x63 is
// the INTEGER 99; 5.0 satisfies `integer`; 5abc satisfies nothing numeric), so
// the emitted JSON Schema is honest about what `--check-config` accepts.

#include "anolis/provider_sdk/config_validate.hpp"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <clocale>
#include <string>
#include <vector>

#include "anolis/provider_sdk/config.hpp"

namespace cfg = anolis::provider_sdk::config;

namespace {

constexpr const char* kIdentPattern = "^[A-Za-z0-9_.-]{1,64}$";

YAML::Node yaml(const std::string& text) { return YAML::Load(text); }

std::vector<cfg::ValidationError> run(const cfg::Schema& schema, const std::string& text) {
    return cfg::validate(schema, yaml(text));
}

bool has_error(const std::vector<cfg::ValidationError>& errors, const std::string& path,
               const std::string& message_fragment) {
    for (const auto& error : errors) {
        if (error.path == path && error.message.find(message_fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string dump(const std::vector<cfg::ValidationError>& errors) { return cfg::format_errors(errors); }

// Compact ezo-like fixture: provider/hardware/discovery/devices with the real
// constraint set (identifier patterns, i2c addresses, uniqueness).
cfg::Schema make_ezo_like_schema() {
    cfg::Object provider(cfg::Openness::Closed);
    provider.field(cfg::string_field("name").pattern(kIdentPattern));

    cfg::Object hardware(cfg::Openness::Closed);
    hardware.field(cfg::string_field("bus_path").required().non_empty());
    hardware.field(cfg::integer_field("query_delay_us").min_int(1));
    hardware.field(cfg::integer_field("timeout_ms").min_int(1));
    hardware.field(cfg::integer_field("retry_count").min_int(0));

    cfg::Object discovery(cfg::Openness::Closed);
    discovery.field(cfg::string_field("mode").required().const_value("manual"));

    cfg::Object device(cfg::Openness::Closed);
    device.field(cfg::string_field("id").required().pattern(kIdentPattern).unique());
    device.field(cfg::string_field("type").required().enum_value("ph").enum_value("do").enum_value("rtd"));
    device.field(cfg::string_field("label").non_empty());
    device.field(cfg::i2c_address_field("address").required().unique());

    cfg::Object root(cfg::Openness::Closed);
    root.child("provider", provider);
    root.child("hardware", hardware, cfg::Presence::Required);
    root.child("discovery", discovery, cfg::Presence::Required);
    root.array("devices", cfg::Array::of_objects(device));
    return cfg::Schema(std::move(root));
}

// Bread-like discovery: mode enum + conditional addresses.
cfg::Schema make_bread_discovery_schema() {
    cfg::Object discovery(cfg::Openness::Closed);
    discovery.field(cfg::string_field("mode").required().enum_value("scan").enum_value("manual"));
    discovery.array("addresses", cfg::Array::of_scalars(cfg::i2c_address_field("address")).min_items(1).unique());
    discovery.when("mode", "manual", cfg::When().require("addresses"));
    discovery.when("mode", "scan", cfg::When().forbid("addresses"));

    cfg::Object root(cfg::Openness::Closed);
    root.child("discovery", discovery, cfg::Presence::Required);
    return cfg::Schema(std::move(root));
}

// Sim-like simulation section: mode-conditional keys + dependent ambient pair
// + deprecated keys.
cfg::Schema make_sim_simulation_schema() {
    cfg::Object simulation(cfg::Openness::Closed);
    simulation.field(
        cfg::string_field("mode").required().enum_value("non_interacting").enum_value("inert").enum_value("sim"));
    simulation.field(cfg::number_field("tick_rate_hz").min_number(0.1).max_number(1000.0));
    simulation.field(cfg::string_field("physics_config").non_empty());
    simulation.field(cfg::number_field("ambient_temp_c"));
    simulation.field(cfg::string_field("ambient_signal_path").non_empty());
    simulation.dependent_required("ambient_signal_path", {"ambient_temp_c"});
    simulation.when("mode", "non_interacting",
                    cfg::When()
                        .require("tick_rate_hz")
                        .forbid("physics_config")
                        .forbid("ambient_temp_c")
                        .forbid("ambient_signal_path"));
    simulation.when("mode", "inert",
                    cfg::When()
                        .forbid("tick_rate_hz")
                        .forbid("physics_config")
                        .forbid("ambient_temp_c")
                        .forbid("ambient_signal_path"));
    simulation.when("mode", "sim", cfg::When().require("tick_rate_hz").require("physics_config"));
    simulation.deprecated_key("noise_enabled", "simulation.noise_enabled is no longer supported");
    simulation.deprecated_key("update_rate_hz", "simulation.update_rate_hz is no longer supported");

    cfg::Object root(cfg::Openness::Open);  // sim's root is open today
    root.child("simulation", simulation, cfg::Presence::Required);
    return cfg::Schema(std::move(root));
}

// ---- the typing seam (extraction helpers = the resolver) -------------------

TEST(ConfigYamlTyping, PlainIntegersResolve) {
    EXPECT_EQ(cfg::as_int64(yaml("v: 300")["v"]), 300);
    EXPECT_EQ(cfg::as_int64(yaml("v: -5")["v"]), -5);
    EXPECT_EQ(cfg::as_int64(yaml("v: +5")["v"]), 5);
    EXPECT_EQ(cfg::as_int64(yaml("v: 0x63")["v"]), 0x63);
    EXPECT_EQ(cfg::as_int64(yaml("v: 0o17")["v"]), 15);  // 0o17 octal == 15
    // YAML 1.2 core: a leading zero does NOT mean octal.
    EXPECT_EQ(cfg::as_int64(yaml("v: 010")["v"]), 10);
}

TEST(ConfigYamlTyping, QuotedScalarsAreStrings) {
    EXPECT_EQ(cfg::as_int64(yaml("v: \"300\"")["v"]), std::nullopt);
    EXPECT_EQ(cfg::as_double(yaml("v: \"0.5\"")["v"]), std::nullopt);
    EXPECT_EQ(cfg::as_bool(yaml("v: \"true\"")["v"]), std::nullopt);
    EXPECT_EQ(cfg::as_string(yaml("v: \"300\"")["v"]), "300");
    EXPECT_EQ(cfg::as_string(yaml("v: '300'")["v"]), "300");
    // ...and a plain numeric is NOT a string.
    EXPECT_EQ(cfg::as_string(yaml("v: 300")["v"]), std::nullopt);
    EXPECT_EQ(cfg::as_string(yaml("v: bus")["v"]), "bus");
}

TEST(ConfigYamlTyping, IntegerAcceptsZeroFractionFloatOnly) {
    EXPECT_EQ(cfg::as_int64(yaml("v: 5.0")["v"]), 5);
    EXPECT_EQ(cfg::as_int64(yaml("v: 5.7")["v"]), std::nullopt);
    // Trailing junk is a STRING under the core schema — never five.
    EXPECT_EQ(cfg::as_int64(yaml("v: 5abc")["v"]), std::nullopt);
    EXPECT_EQ(cfg::as_string(yaml("v: 5abc")["v"]), "5abc");
}

TEST(ConfigYamlTyping, Doubles) {
    EXPECT_EQ(cfg::as_double(yaml("v: 0.5")["v"]), 0.5);
    EXPECT_EQ(cfg::as_double(yaml("v: 5e-3")["v"]), 5e-3);
    EXPECT_EQ(cfg::as_double(yaml("v: 5")["v"]), 5.0);
    EXPECT_EQ(cfg::as_double(yaml("v: .5")["v"]), 0.5);
    EXPECT_TRUE(std::isinf(cfg::as_double(yaml("v: .inf")["v"]).value()));
    EXPECT_TRUE(std::isnan(cfg::as_double(yaml("v: .nan")["v"]).value()));
}

TEST(ConfigYamlTyping, BooleansAreYaml12CoreOnly) {
    EXPECT_EQ(cfg::as_bool(yaml("v: true")["v"]), true);
    EXPECT_EQ(cfg::as_bool(yaml("v: False")["v"]), false);
    EXPECT_EQ(cfg::as_bool(yaml("v: TRUE")["v"]), true);
    // YAML 1.1-isms are NOT booleans under the 1.2 core schema.
    EXPECT_EQ(cfg::as_bool(yaml("v: yes")["v"]), std::nullopt);
    EXPECT_EQ(cfg::as_bool(yaml("v: on")["v"]), std::nullopt);
    EXPECT_EQ(cfg::as_string(yaml("v: yes")["v"]), "yes");
}

TEST(ConfigYamlTyping, NullsAndNonScalars) {
    EXPECT_EQ(cfg::resolved_kind(yaml("v: ~")["v"]), cfg::ScalarKind::Null);
    EXPECT_EQ(cfg::resolved_kind(yaml("v: null")["v"]), cfg::ScalarKind::Null);
    EXPECT_EQ(cfg::resolved_kind(yaml("v:")["v"]), cfg::ScalarKind::Null);
    EXPECT_EQ(cfg::resolved_kind(yaml("v: {a: 1}")["v"]), cfg::ScalarKind::NonScalar);
    EXPECT_EQ(cfg::resolved_kind(yaml("v: [1]")["v"]), cfg::ScalarKind::NonScalar);
    EXPECT_EQ(cfg::resolved_kind(yaml("v: 1")["missing"]), cfg::ScalarKind::NonScalar);
}

TEST(ConfigYamlTyping, I2cAddressForms) {
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: 0x61")["v"]), 0x61);
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: 97")["v"]), 97);
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: \"0x61\"")["v"]), 0x61);
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: \"0X0F\"")["v"]), 0x0F);
    // A quoted DECIMAL string is not an address (strict form: 0xNN only).
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: \"97\"")["v"]), std::nullopt);
    // Range is enforced on both branches.
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: 0x07")["v"]), std::nullopt);
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: 0x78")["v"]), std::nullopt);
    EXPECT_EQ(cfg::parse_i2c_address(yaml("v: \"0x78\"")["v"]), std::nullopt);
}

// ---- validate(): structure -------------------------------------------------

TEST(ConfigValidate, NonMapRootFails) {
    const auto errors = run(make_ezo_like_schema(), "- a\n- b");
    EXPECT_TRUE(has_error(errors, "(root)", "must be a map")) << dump(errors);
}

TEST(ConfigValidate, RequiredMembersEnforced) {
    const auto errors = run(make_ezo_like_schema(), "hardware:\n  bus_path: /dev/i2c-1\n");
    EXPECT_TRUE(has_error(errors, "discovery", "is required")) << dump(errors);
}

TEST(ConfigValidate, UnknownKeyRejectedOnClosedObject) {
    const auto errors =
        run(make_ezo_like_schema(), "hardware:\n  bus_path: /dev/i2c-1\n  bogus: 1\ndiscovery:\n  mode: manual\n");
    EXPECT_TRUE(has_error(errors, "hardware.bogus", "unknown key")) << dump(errors);
}

TEST(ConfigValidate, OpenObjectAcceptsUnknownKeys) {
    const auto errors = run(make_sim_simulation_schema(), "startup_policy: strict\nsimulation:\n  mode: inert\n");
    EXPECT_TRUE(errors.empty()) << dump(errors);
}

TEST(ConfigValidate, DeprecatedKeyGetsItsMessage) {
    const auto errors = run(make_sim_simulation_schema(), "simulation:\n  mode: inert\n  noise_enabled: true\n");
    EXPECT_TRUE(has_error(errors, "simulation.noise_enabled", "no longer supported")) << dump(errors);
}

TEST(ConfigValidate, CollectsAllErrorsNotJustTheFirst) {
    const auto errors = run(make_ezo_like_schema(), "hardware:\n  timeout_ms: 0\ndiscovery:\n  mode: automatic\n");
    // bus_path missing + timeout_ms below minimum + mode not 'manual' = 3.
    EXPECT_TRUE(has_error(errors, "hardware.bus_path", "is required")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "hardware.timeout_ms", ">= 1")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "discovery.mode", "must be 'manual'")) << dump(errors);
    EXPECT_EQ(errors.size(), 3U) << dump(errors);
}

TEST(ConfigValidate, FormatErrorsJoinsLines) {
    const std::vector<cfg::ValidationError> errors = {{"a.b", "is required"}, {"c", "unknown key"}};
    EXPECT_EQ(cfg::format_errors(errors), "a.b: is required\nc: unknown key");
}

// ---- validate(): the typing seam at field level ----------------------------

TEST(ConfigValidate, QuotedNumericIsATypeError) {
    const auto errors = run(make_ezo_like_schema(),
                            "hardware:\n  bus_path: /dev/i2c-1\n  timeout_ms: \"300\"\ndiscovery:\n  mode: manual\n");
    EXPECT_TRUE(has_error(errors, "hardware.timeout_ms", "must be an integer, got a string")) << dump(errors);
}

TEST(ConfigValidate, StoiStyleTrailingJunkIsATypeError) {
    // `timeout_ms: 5abc` parsed as 5 under the old stoi-based providers; the
    // schema-honest validator rejects it as a string.
    const auto errors = run(make_ezo_like_schema(),
                            "hardware:\n  bus_path: /dev/i2c-1\n  timeout_ms: 5abc\ndiscovery:\n  mode: manual\n");
    EXPECT_TRUE(has_error(errors, "hardware.timeout_ms", "must be an integer")) << dump(errors);
}

TEST(ConfigValidate, PlainIntegerBusPathIsATypeError) {
    const auto errors = run(make_ezo_like_schema(), "hardware:\n  bus_path: 123\ndiscovery:\n  mode: manual\n");
    EXPECT_TRUE(has_error(errors, "hardware.bus_path", "must be a string, got an integer")) << dump(errors);
}

TEST(ConfigValidate, NullValueIsATypeError) {
    const auto errors = run(make_ezo_like_schema(), "hardware:\n  bus_path:\ndiscovery:\n  mode: manual\n");
    EXPECT_TRUE(has_error(errors, "hardware.bus_path", "must be a string, got null")) << dump(errors);
}

// ---- validate(): field constraints -----------------------------------------

TEST(ConfigValidate, StringConstraints) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("id").pattern("^[a-z]+$").forbid_value("reserved", "'reserved' is reserved"));
    root.field(cfg::string_field("mode").enum_value("a").enum_value("b"));
    root.field(cfg::string_field("path").non_empty());
    const cfg::Schema schema{std::move(root)};

    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("id: UPPER")), "id", "must match"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("id: reserved")), "id", "'reserved' is reserved"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("mode: c")), "mode", "valid values: a, b"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("path: \"\"")), "path", "must not be empty"));
    EXPECT_TRUE(cfg::validate(schema, yaml("id: ok\nmode: a\npath: x")).empty());
}

TEST(ConfigValidate, NumberBoundsIncludingExclusive) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::number_field("tick").min_number(0.1).max_number(1000.0));
    root.field(cfg::number_field("speed").exclusive_min_number(0.0).max_number(10000.0));
    const cfg::Schema schema{std::move(root)};

    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("tick: 0.05")), "tick", ">= 0.1"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("tick: 1001")), "tick", "<= 1000"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("speed: 0")), "speed", "> 0"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("speed: 0.0")), "speed", "> 0"));
    EXPECT_TRUE(cfg::validate(schema, yaml("tick: 10\nspeed: 0.001")).empty());
}

TEST(ConfigValidate, NonFiniteNumberRejected) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::number_field("x"));
    const cfg::Schema schema{std::move(root)};
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("x: .inf")), "x", "finite"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("x: .nan")), "x", "finite"));
}

TEST(ConfigValidate, BooleanField) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::boolean_field("enabled"));
    const cfg::Schema schema{std::move(root)};
    EXPECT_TRUE(cfg::validate(schema, yaml("enabled: true")).empty());
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("enabled: yes")), "enabled", "must be a boolean"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("enabled: 1")), "enabled", "must be a boolean"));
}

TEST(ConfigValidate, I2cAddressField) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::i2c_address_field("address"));
    const cfg::Schema schema{std::move(root)};
    EXPECT_TRUE(cfg::validate(schema, yaml("address: 0x61")).empty());
    EXPECT_TRUE(cfg::validate(schema, yaml("address: 97")).empty());
    EXPECT_TRUE(cfg::validate(schema, yaml("address: \"0x61\"")).empty());
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("address: 0x78")), "address", "0x08-0x77"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("address: \"97\"")), "address", "0x08-0x77"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("address: true")), "address", "integer or 0x-prefixed"));
}

// ---- validate(): arrays -----------------------------------------------------

TEST(ConfigValidate, ArrayShapeAndBounds) {
    cfg::Object root(cfg::Openness::Closed);
    root.array("xs", cfg::Array::of_scalars(cfg::integer_field("x")).min_items(1).max_items(3));
    const cfg::Schema schema{std::move(root)};
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("xs: 5")), "xs", "must be a sequence"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("xs: []")), "xs", "at least 1"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("xs: [1, 2, 3, 4]")), "xs", "at most 3"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("xs: [1, two]")), "xs[1]", "must be an integer"));
}

TEST(ConfigValidate, ScalarArrayUniquenessComparesParsedValues) {
    cfg::Object root(cfg::Openness::Closed);
    root.array("addresses", cfg::Array::of_scalars(cfg::i2c_address_field("address")).unique());
    const cfg::Schema schema{std::move(root)};
    // 0x08 (int) and "0x08" (hex string) are DIFFERENT JSON values but the
    // SAME parsed address — native uniqueItems would miss this.
    const auto errors = cfg::validate(schema, yaml("addresses: [0x08, \"0x08\"]"));
    EXPECT_TRUE(has_error(errors, "addresses[1]", "duplicate")) << dump(errors);
    EXPECT_TRUE(cfg::validate(schema, yaml("addresses: [0x08, 0x09]")).empty());
}

TEST(ConfigValidate, ObjectArrayPerFieldUniqueness) {
    const auto schema = make_ezo_like_schema();
    const std::string doc =
        "hardware:\n  bus_path: /dev/i2c-1\n"
        "discovery:\n  mode: manual\n"
        "devices:\n"
        "  - {id: ph1, type: ph, address: 0x63}\n"
        "  - {id: ph1, type: do, address: 0x61}\n"
        "  - {id: do1, type: do, address: \"0x63\"}\n";
    const auto errors = run(schema, doc);
    EXPECT_TRUE(has_error(errors, "devices[1].id", "duplicate")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "devices[2].address", "duplicate")) << dump(errors);
}

// ---- validate(): conditionals + dependents ----------------------------------

TEST(ConfigValidate, DiscriminatorBranchesApply) {
    const auto schema = make_bread_discovery_schema();
    EXPECT_TRUE(cfg::validate(schema, yaml("discovery:\n  mode: manual\n  addresses: [0x08]")).empty());
    EXPECT_TRUE(cfg::validate(schema, yaml("discovery:\n  mode: scan")).empty());
    {
        const auto errors = run(schema, "discovery:\n  mode: manual\n");
        EXPECT_TRUE(has_error(errors, "discovery.addresses", "required when mode is 'manual'")) << dump(errors);
    }
    {
        const auto errors = run(schema, "discovery:\n  mode: scan\n  addresses: [0x08]\n");
        EXPECT_TRUE(has_error(errors, "discovery.addresses", "not valid when mode is 'scan'")) << dump(errors);
    }
}

TEST(ConfigValidate, AbsentDiscriminatorFiresNoBranch) {
    // Only the discriminator's own `is required` may fire — never a branch
    // (mirrors the emitted `if` carrying `required: [mode]`).
    const auto errors = run(make_bread_discovery_schema(), "discovery: {}\n");
    EXPECT_TRUE(has_error(errors, "discovery.mode", "is required")) << dump(errors);
    EXPECT_EQ(errors.size(), 1U) << dump(errors);
}

TEST(ConfigValidate, SimModeMatrix) {
    const auto schema = make_sim_simulation_schema();
    EXPECT_TRUE(cfg::validate(schema, yaml("simulation:\n  mode: inert")).empty());
    EXPECT_TRUE(cfg::validate(schema, yaml("simulation:\n  mode: non_interacting\n  tick_rate_hz: 10")).empty());
    {
        const auto errors = run(schema, "simulation:\n  mode: inert\n  tick_rate_hz: 10\n");
        EXPECT_TRUE(has_error(errors, "simulation.tick_rate_hz", "not valid when mode is 'inert'")) << dump(errors);
    }
    {
        const auto errors = run(schema, "simulation:\n  mode: non_interacting\n");
        EXPECT_TRUE(has_error(errors, "simulation.tick_rate_hz", "required when mode is 'non_interacting'"))
            << dump(errors);
    }
    {
        const auto errors = run(schema, "simulation:\n  mode: sim\n  tick_rate_hz: 10\n");
        EXPECT_TRUE(has_error(errors, "simulation.physics_config", "required when mode is 'sim'")) << dump(errors);
    }
}

TEST(ConfigValidate, DependentRequired) {
    const auto schema = make_sim_simulation_schema();
    const auto errors = run(schema,
                            "simulation:\n  mode: sim\n  tick_rate_hz: 10\n  physics_config: p.yaml\n  "
                            "ambient_signal_path: reactor.temp\n");
    EXPECT_TRUE(has_error(errors, "simulation.ambient_temp_c", "required when ambient_signal_path is present"))
        << dump(errors);
}

// ---- fixtures: a real-world-shaped config end to end ------------------------

TEST(ConfigValidate, EzoLikeHappyPath) {
    const std::string doc =
        "provider:\n  name: ezo0\n"
        "hardware:\n  bus_path: mock://bench\n  query_delay_us: 300000\n  timeout_ms: 300\n  retry_count: 2\n"
        "discovery:\n  mode: manual\n"
        "devices:\n"
        "  - {id: ph1, type: ph, address: 0x63, label: pH probe}\n"
        "  - {id: rtd1, type: rtd, address: 0x66}\n";
    const auto errors = run(make_ezo_like_schema(), doc);
    EXPECT_TRUE(errors.empty()) << dump(errors);
}

// ---- review-driven regressions (adversarial review of PR 2) ----------------

TEST(ConfigYamlTyping, ZeroFractionFloatIsIntegerEverywhere) {
    // 1e16 has a zero fraction: `type: integer` accepts it (JSON Schema
    // semantics), and it is exactly representable in int64.
    EXPECT_EQ(cfg::as_int64(yaml("v: 1e16")["v"]), 10000000000000000LL);
    // Same number, different spelling — identical verdict.
    EXPECT_EQ(cfg::as_int64(yaml("v: 10000000000000000")["v"]), 10000000000000000LL);
    // Above int64: rejected (the documented deviation).
    EXPECT_EQ(cfg::as_int64(yaml("v: 1e300")["v"]), std::nullopt);
}

TEST(ConfigValidate, I2cAddressAcceptsZeroFractionFloat) {
    // The emitted anyOf int branch is `type: integer`, which 8.0 satisfies —
    // the validator must agree.
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::i2c_address_field("address"));
    const cfg::Schema schema{std::move(root)};
    EXPECT_TRUE(cfg::validate(schema, yaml("address: 8.0")).empty());
    EXPECT_TRUE(cfg::validate(schema, yaml("address: 119.0")).empty());
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("address: 7.0")), "address", "0x08-0x77"));
    EXPECT_TRUE(has_error(cfg::validate(schema, yaml("address: 8.5")), "address", "integer or 0x-prefixed"));
}

TEST(ConfigValidate, DuplicateMapKeysRejected) {
    // yaml-cpp is first-wins, PyYAML last-wins, js-yaml throws — an ambiguous
    // document must not validate (and the second value must not be blessed).
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::integer_field("x"));
    const cfg::Schema schema{std::move(root)};
    const auto errors = cfg::validate(schema, yaml("x: 1\nx: oops"));
    EXPECT_TRUE(has_error(errors, "x", "duplicate key")) << dump(errors);
    // ...and a duplicated unknown key errors once each way, not twice.
    const auto unknown = cfg::validate(schema, yaml("x: 1\ny: 1\ny: 2"));
    EXPECT_TRUE(has_error(unknown, "y", "duplicate key")) << dump(unknown);
    EXPECT_EQ(unknown.size(), 2U) << dump(unknown);  // duplicate + one unknown-key
}

TEST(ConfigValidate, NegativeZeroCollidesInUniqueArrays) {
    // Draft 2020-12 uniqueItems treats 0.0 and -0.0 as equal.
    cfg::Object root(cfg::Openness::Closed);
    root.array("xs", cfg::Array::of_scalars(cfg::number_field("x")).unique());
    const cfg::Schema schema{std::move(root)};
    const auto errors = cfg::validate(schema, yaml("xs: [0.0, -0.0]"));
    EXPECT_TRUE(has_error(errors, "xs[1]", "duplicate")) << dump(errors);
}

TEST(ConfigYamlTyping, ResolutionIsLocaleIndependent) {
    // std::stod would honor LC_NUMERIC; the resolver must not.
    const char* previous = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
    if (previous == nullptr) {
        GTEST_SKIP() << "de_DE.UTF-8 locale not installed on this host";
    }
    EXPECT_EQ(cfg::as_double(yaml("v: 5.7")["v"]), 5.7);
    EXPECT_EQ(cfg::resolved_kind(yaml("v: 5.7")["v"]), cfg::ScalarKind::Float);
    std::setlocale(LC_NUMERIC, "C");
}

TEST(ConfigYamlTyping, FloatOverflowAndUnderflow) {
    // Overflow resolves to ±inf (a Number field rejects it as non-finite);
    // underflow resolves to ±0 like strtod.
    EXPECT_TRUE(std::isinf(cfg::as_double(yaml("v: 1e999")["v"]).value()));
    EXPECT_LT(cfg::as_double(yaml("v: -1e999")["v"]).value(), 0.0);
    EXPECT_EQ(cfg::as_double(yaml("v: 1e-999")["v"]), 0.0);
}

TEST(ConfigValidate, ValidationErrorsCarryFullPaths) {
    const std::string doc =
        "hardware:\n  bus_path: mock://bench\n"
        "discovery:\n  mode: manual\n"
        "devices:\n"
        "  - {id: 'bad id!', type: fake, address: 0x99}\n";
    const auto errors = run(make_ezo_like_schema(), doc);
    EXPECT_TRUE(has_error(errors, "devices[0].id", "must match")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "devices[0].type", "invalid value 'fake'")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "devices[0].address", "0x08-0x77")) << dump(errors);
}

}  // namespace
