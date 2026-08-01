// Config-schema toolkit (declare-once, sdk#24) — declaration model, JSON
// Schema emission, and the `--config-schema` envelope.
//
// The emitted JSON's *well-formedness* oracle is protobuf's strict JSON parser
// (google::protobuf::Struct) — already a dependency. Struct loses int-vs-float
// and silently last-wins duplicate keys, so the integer-ness of
// `config_schema_version` and key-shape guarantees are asserted on the RAW
// text (golden test), and duplicate keys are prevented at declaration time
// (builder throws) rather than detected at parse time.

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>

#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "anolis/provider_sdk/config.hpp"

namespace cfg = anolis::provider_sdk::config;

// The Hardening lane's tsan-triplet (dynamic) protobuf mis-parses JSON into an
// empty Struct (OK status, zero fields) — upstream interaction tracked in
// sdk#26. The Struct-oracle tests are meaningless there and are skipped; the
// required Linux/Windows lanes run them, and the golden-text tests (raw bytes,
// no parse) still run under TSAN.
#if defined(__SANITIZE_THREAD__)
#define ANOLIS_SKIP_ORACLE_UNDER_TSAN() GTEST_SKIP() << "Struct JSON-parse oracle unavailable under TSAN (sdk#26)"
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ANOLIS_SKIP_ORACLE_UNDER_TSAN() GTEST_SKIP() << "Struct JSON-parse oracle unavailable under TSAN (sdk#26)"
#endif
#endif
#ifndef ANOLIS_SKIP_ORACLE_UNDER_TSAN
#define ANOLIS_SKIP_ORACLE_UNDER_TSAN() static_cast<void>(0)
#endif

namespace {

constexpr const char* kIdentPattern = "^[A-Za-z0-9_.-]{1,64}$";

// -Wmissing-field-initializers fires on partial designated initializers, so
// options are built via helpers.
cfg::EnvelopeOptions with_provider(std::string provider_name) {
    cfg::EnvelopeOptions options;
    options.provider_name = std::move(provider_name);
    return options;
}

cfg::EnvelopeOptions with_extras(std::string provider_name, std::vector<std::pair<std::string, std::string>> extras) {
    cfg::EnvelopeOptions options;
    options.provider_name = std::move(provider_name);
    options.extra_string_entries = std::move(extras);
    return options;
}

google::protobuf::Struct parse_json(const std::string& text) {
    google::protobuf::Struct parsed;
    const auto status = google::protobuf::util::JsonStringToMessage(text, &parsed);
    EXPECT_TRUE(status.ok()) << "emitted JSON must parse: " << status.ToString() << "\n" << text;
    return parsed;
}

const google::protobuf::Value* find_field(const google::protobuf::Struct& obj, const std::string& key) {
    const auto it = obj.fields().find(key);
    return it == obj.fields().end() ? nullptr : &it->second;
}

// Navigate `path` of object keys from `root`; nullptr when absent.
const google::protobuf::Value* find_path(const google::protobuf::Struct& root, const std::vector<std::string>& path) {
    const google::protobuf::Struct* current = &root;
    const google::protobuf::Value* value = nullptr;
    for (const auto& key : path) {
        value = find_field(*current, key);
        if (value == nullptr) {
            return nullptr;
        }
        if (value->kind_case() == google::protobuf::Value::kStructValue) {
            current = &value->struct_value();
        }
    }
    return value;
}

// Null-safe navigation: a missing key is a test FAILURE, never a deref crash.
const google::protobuf::Value& at(const google::protobuf::Struct& root, const std::vector<std::string>& path) {
    static const google::protobuf::Value kMissing;
    const auto* value = find_path(root, path);
    if (value == nullptr) {
        ADD_FAILURE() << "missing path element: " << (path.empty() ? "<root>" : path.back());
        return kMissing;
    }
    return *value;
}

const google::protobuf::Value& at(const google::protobuf::Struct& obj, const std::string& key) {
    static const google::protobuf::Value kMissing;
    const auto* value = find_field(obj, key);
    if (value == nullptr) {
        ADD_FAILURE() << "missing key: " << key;
        return kMissing;
    }
    return *value;
}

// A realistic fixture mirroring anolis-provider-ezo's config surface.
cfg::Schema make_ezo_like_schema() {
    cfg::Object provider(cfg::Openness::Closed);
    provider.field(cfg::string_field("name").pattern(kIdentPattern).title("Provider name").default_string("ezo0"));

    cfg::Object hardware(cfg::Openness::Closed);
    hardware.field(cfg::string_field("bus_path").required().non_empty().placeholder("/dev/i2c-1 or mock://name"));
    hardware.field(cfg::integer_field("query_delay_us").min_int(1).default_int(300000));
    hardware.field(cfg::integer_field("timeout_ms").min_int(1).default_int(300));
    hardware.field(cfg::integer_field("retry_count").min_int(0).default_int(2));

    cfg::Object discovery(cfg::Openness::Closed);
    discovery.field(cfg::string_field("mode").required().const_value("manual"));

    cfg::Object device(cfg::Openness::Closed);
    device.field(cfg::string_field("id").required().pattern(kIdentPattern).unique());
    device.field(cfg::string_field("type")
                     .required()
                     .enum_value("ph", "pH Sensor")
                     .enum_value("do", "Dissolved Oxygen")
                     .enum_value("ec", "Conductivity")
                     .enum_value("orp", "ORP")
                     .enum_value("rtd", "Temperature (RTD)")
                     .enum_value("hum", "Humidity"));
    device.field(cfg::string_field("label").non_empty().description("Defaults to the device id."));
    device.field(cfg::i2c_address_field("address").required().unique());

    cfg::Object root(cfg::Openness::Closed);
    root.child("provider", provider);
    root.child("hardware", hardware, cfg::Presence::Required);
    root.child("discovery", discovery, cfg::Presence::Required);
    root.array("devices", cfg::Array::of_objects(device));

    return cfg::Schema(std::move(root)).title("anolis-provider-ezo configuration");
}

// ---- envelope ------------------------------------------------------------

TEST(ConfigSchemaEnvelope, GoldenText) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("name").required().pattern("^[a-z]+$"));
    const cfg::Schema schema{std::move(root)};

    const std::string text = cfg::config_schema_envelope(schema, with_provider("anolis-provider-test"));
    EXPECT_EQ(text,
              "{\n"
              "  \"config_schema_version\": 1,\n"
              "  \"provider\": \"anolis-provider-test\",\n"
              "  \"schema\": {\n"
              "    \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n"
              "    \"type\": \"object\",\n"
              "    \"properties\": {\n"
              "      \"name\": {\n"
              "        \"type\": \"string\",\n"
              "        \"pattern\": \"^[a-z]+$\"\n"
              "      }\n"
              "    },\n"
              "    \"required\": [\n"
              "      \"name\"\n"
              "    ],\n"
              "    \"additionalProperties\": false\n"
              "  }\n"
              "}");
}

TEST(ConfigSchemaEnvelope, WriteAppendsTrailingNewline) {
    const cfg::Schema schema{cfg::Object(cfg::Openness::Open)};
    std::ostringstream out;
    cfg::write_config_schema_envelope(out, schema, {});
    const std::string text = out.str();
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.substr(0, text.size() - 1), cfg::config_schema_envelope(schema, {}));
}

// Mirrors the protocol conformance validator's envelope assertions
// (anolis-protocol conformance/anolis_conformance/checks.py,
// assert_config_schema_envelope): parseable JSON object; integer
// config_schema_version >= 1 (not bool, not float); `schema` a JSON object.
TEST(ConfigSchemaEnvelope, MirrorsConformanceAssertions) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    const auto envelope = parse_json(cfg::config_schema_envelope(make_ezo_like_schema(), with_provider("ezo")));

    const auto* version = find_field(envelope, "config_schema_version");
    ASSERT_NE(version, nullptr);
    ASSERT_EQ(version->kind_case(), google::protobuf::Value::kNumberValue);
    EXPECT_EQ(version->number_value(), 1.0);

    const auto* schema = find_field(envelope, "schema");
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->kind_case(), google::protobuf::Value::kStructValue);

    const auto* provider = find_field(envelope, "provider");
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->string_value(), "ezo");
}

// Struct parsing loses int-vs-float, and Python's json.loads("1.0") is a float
// the conformance validator rejects — so the integer literal is asserted on the
// raw text.
TEST(ConfigSchemaEnvelope, VersionIsARawIntegerLiteral) {
    const std::string text = cfg::config_schema_envelope(cfg::Schema{cfg::Object(cfg::Openness::Open)}, {});
    EXPECT_NE(text.find("\"config_schema_version\": 1,\n"), std::string::npos);
}

TEST(ConfigSchemaEnvelope, OmitsProviderWhenEmpty) {
    const std::string text = cfg::config_schema_envelope(cfg::Schema{cfg::Object(cfg::Openness::Open)}, {});
    EXPECT_EQ(text.find("\"provider\""), std::string::npos);
}

TEST(ConfigSchemaEnvelope, ExtraStringEntries) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    const cfg::Schema schema{cfg::Object(cfg::Openness::Open)};
    const std::string text = cfg::config_schema_envelope(schema, with_extras("p", {{"provider_version", "1.2.3"}}));
    const auto envelope = parse_json(text);
    const auto* extra = find_field(envelope, "provider_version");
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->string_value(), "1.2.3");
}

TEST(ConfigSchemaEnvelope, ExtraEntryCollidingWithReservedKeyThrows) {
    const cfg::Schema schema{cfg::Object(cfg::Openness::Open)};
    EXPECT_THROW(cfg::config_schema_envelope(schema, with_extras("", {{"schema", "x"}})), std::logic_error);
    EXPECT_THROW(cfg::config_schema_envelope(schema, with_extras("", {{"config_schema_version", "2"}})),
                 std::logic_error);
    EXPECT_THROW(cfg::config_schema_envelope(schema, with_extras("p", {{"provider", "q"}})), std::logic_error);
    EXPECT_THROW(cfg::config_schema_envelope(schema, with_extras("", {{"a", "1"}, {"a", "2"}})), std::logic_error);
}

// ---- schema-level metadata -----------------------------------------------

TEST(ConfigSchemaEmission, SchemaIdTitleDescription) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Open);
    root.title("object title");
    const auto schema =
        cfg::Schema{std::move(root)}.id("urn:example:config-schema:2").title("schema title").description("desc");
    const auto parsed = parse_json(cfg::to_json_schema(schema));

    const auto* id = find_field(parsed, "$id");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->string_value(), "urn:example:config-schema:2");
    const auto* dialect = find_field(parsed, "$schema");
    ASSERT_NE(dialect, nullptr);
    EXPECT_EQ(dialect->string_value(), "https://json-schema.org/draft/2020-12/schema");
    // Schema-level title overrides the root object's own.
    const auto* title = find_field(parsed, "title");
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->string_value(), "schema title");
}

// ---- field emission ------------------------------------------------------

TEST(ConfigSchemaEmission, EnumWithoutTitlesEmitsEnum) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("mode").enum_value("strict").enum_value("degraded"));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    const auto* values = find_path(parsed, {"properties", "mode", "enum"});
    ASSERT_NE(values, nullptr);
    ASSERT_EQ(values->list_value().values_size(), 2);
    EXPECT_EQ(values->list_value().values(0).string_value(), "strict");
    EXPECT_EQ(find_path(parsed, {"properties", "mode", "oneOf"}), nullptr);
}

TEST(ConfigSchemaEmission, EnumWithTitlesEmitsOneOfConstTitle) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("type").enum_value("ph", "pH Sensor").enum_value("do"));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    const auto* one_of = find_path(parsed, {"properties", "type", "oneOf"});
    ASSERT_NE(one_of, nullptr);
    ASSERT_EQ(one_of->list_value().values_size(), 2);
    const auto& first = one_of->list_value().values(0).struct_value();
    EXPECT_EQ(at(first, "const").string_value(), "ph");
    EXPECT_EQ(at(first, "title").string_value(), "pH Sensor");
    const auto& second = one_of->list_value().values(1).struct_value();
    EXPECT_EQ(at(second, "const").string_value(), "do");
    EXPECT_EQ(find_field(second, "title"), nullptr);
}

TEST(ConfigSchemaEmission, SingleForbiddenValueEmitsNotConst) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("id").forbid_value("chaos_control", "reserved"));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    const auto* not_const = find_path(parsed, {"properties", "id", "not", "const"});
    ASSERT_NE(not_const, nullptr);
    EXPECT_EQ(not_const->string_value(), "chaos_control");
}

TEST(ConfigSchemaEmission, MultipleForbiddenValuesEmitNotEnum) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("id").forbid_value("a").forbid_value("b"));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    const auto* not_enum = find_path(parsed, {"properties", "id", "not", "enum"});
    ASSERT_NE(not_enum, nullptr);
    EXPECT_EQ(not_enum->list_value().values_size(), 2);
}

TEST(ConfigSchemaEmission, NonEmptyEmitsMinLength) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("bus_path").non_empty());
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    const auto* min_length = find_path(parsed, {"properties", "bus_path", "minLength"});
    ASSERT_NE(min_length, nullptr);
    EXPECT_EQ(min_length->number_value(), 1.0);
}

TEST(ConfigSchemaEmission, IntegerBoundsAndDefaultStayIntegral) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::integer_field("timeout_ms").min_int(1).max_int(60000).default_int(300));
    const std::string text = cfg::to_json_schema(cfg::Schema{std::move(root)});
    // Raw-text asserts pinned to the value's end (",\n" / "\n"): integer
    // fidelity must survive emission — "1.0" would not match.
    EXPECT_NE(text.find("\"minimum\": 1,\n"), std::string::npos);
    EXPECT_NE(text.find("\"maximum\": 60000,\n"), std::string::npos);
    EXPECT_NE(text.find("\"default\": 300\n"), std::string::npos);
}

TEST(ConfigSchemaEmission, NumberBoundsEmitDoubles) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::number_field("tick_rate_hz").min_number(0.1).max_number(1000.0).default_number(10.0));
    const std::string text = cfg::to_json_schema(cfg::Schema{std::move(root)});
    EXPECT_NE(text.find("\"minimum\": 0.1"), std::string::npos);
    // Integral doubles serialize without a decimal point (shortest round-trip);
    // JSON Schema `type: number` accepts integer literals.
    EXPECT_NE(text.find("\"maximum\": 1000"), std::string::npos);
}

TEST(ConfigSchemaEmission, BooleanFieldWithDefault) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::boolean_field("enabled").default_bool(true));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    EXPECT_EQ(at(parsed, {"properties", "enabled", "type"}).string_value(), "boolean");
    EXPECT_TRUE(at(parsed, {"properties", "enabled", "default"}).bool_value());
}

TEST(ConfigSchemaEmission, PlaceholderEmitsAnnotation) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("bus_path").placeholder("/dev/i2c-1 or mock://name"));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    const auto* placeholder = find_path(parsed, {"properties", "bus_path", "x-anolis-placeholder"});
    ASSERT_NE(placeholder, nullptr);
    EXPECT_EQ(placeholder->string_value(), "/dev/i2c-1 or mock://name");
}

TEST(ConfigSchemaEmission, I2cAddressShape) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::i2c_address_field("address").default_int(0x61));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    const auto* any_of = find_path(parsed, {"properties", "address", "anyOf"});
    ASSERT_NE(any_of, nullptr);
    ASSERT_EQ(any_of->list_value().values_size(), 2);
    const auto& int_branch = any_of->list_value().values(0).struct_value();
    EXPECT_EQ(at(int_branch, "type").string_value(), "integer");
    EXPECT_EQ(at(int_branch, "minimum").number_value(), 8.0);
    EXPECT_EQ(at(int_branch, "maximum").number_value(), 119.0);
    const auto& string_branch = any_of->list_value().values(1).struct_value();
    EXPECT_EQ(at(string_branch, "type").string_value(), "string");

    EXPECT_EQ(at(parsed, {"properties", "address", "x-anolis-type"}).string_value(), "i2c_address");
    EXPECT_EQ(at(parsed, {"properties", "address", "default"}).number_value(), 97.0);
}

TEST(ConfigSchemaEmission, I2cHexPatternMatchesExactlyTheAddressableRange) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::i2c_address_field("address"));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    const auto* pattern_value = find_path(parsed, {"properties", "address", "anyOf"});
    ASSERT_NE(pattern_value, nullptr);
    const auto& string_branch = pattern_value->list_value().values(1).struct_value();
    const std::regex pattern(at(string_branch, "pattern").string_value(), std::regex::ECMAScript);

    for (const char* accepted : {"0x08", "0x61", "0x77", "0X0f", "0x10", "0x6F", "0x70"}) {
        EXPECT_TRUE(std::regex_search(accepted, pattern)) << accepted;
    }
    for (const char* rejected : {"0x07", "0x78", "0x80", "0xFF", "97", "0x", "0x081", "x61", "0y61"}) {
        EXPECT_FALSE(std::regex_search(rejected, pattern)) << rejected;
    }
}

// ---- object / array emission ---------------------------------------------

TEST(ConfigSchemaEmission, ClosedObjectEmitsAdditionalPropertiesFalse) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{cfg::Object(cfg::Openness::Closed)}));
    const auto* additional = find_field(parsed, "additionalProperties");
    ASSERT_NE(additional, nullptr);
    EXPECT_FALSE(additional->bool_value());
}

TEST(ConfigSchemaEmission, OpenObjectOmitsAdditionalProperties) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{cfg::Object(cfg::Openness::Open)}));
    EXPECT_EQ(find_field(parsed, "additionalProperties"), nullptr);
}

TEST(ConfigSchemaEmission, UniqueScalarArray) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.array("addresses", cfg::Array::of_scalars(cfg::i2c_address_field("address")).min_items(1).unique());
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    EXPECT_NE(find_path(parsed, {"properties", "addresses", "items", "anyOf"}), nullptr);
    EXPECT_EQ(at(parsed, {"properties", "addresses", "minItems"}).number_value(), 1.0);
    EXPECT_TRUE(at(parsed, {"properties", "addresses", "uniqueItems"}).bool_value());
    EXPECT_TRUE(at(parsed, {"properties", "addresses", "x-anolis-unique"}).bool_value());
}

TEST(ConfigSchemaEmission, UniqueFieldInsideObjectArrayEmitsAnnotation) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    const auto parsed = parse_json(cfg::to_json_schema(make_ezo_like_schema()));
    const auto* unique = find_path(parsed, {"properties", "devices", "items", "properties", "id", "x-anolis-unique"});
    ASSERT_NE(unique, nullptr);
    EXPECT_TRUE(unique->bool_value());
}

TEST(ConfigSchemaEmission, DependentRequired) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::number_field("ambient_temp_c"));
    root.field(cfg::string_field("ambient_signal_path").non_empty());
    root.dependent_required("ambient_signal_path", {"ambient_temp_c"});
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    const auto* dependent = find_path(parsed, {"dependentRequired", "ambient_signal_path"});
    ASSERT_NE(dependent, nullptr);
    ASSERT_EQ(dependent->list_value().values_size(), 1);
    EXPECT_EQ(dependent->list_value().values(0).string_value(), "ambient_temp_c");
}

// The `if` MUST carry `required: [discriminator]`: `properties` alone is
// vacuously satisfied when the discriminator is absent, which would fire every
// branch's `then` simultaneously on an invalid document.
TEST(ConfigSchemaEmission, ConditionalIfRequiresDiscriminator) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("mode").required().enum_value("scan").enum_value("manual"));
    root.array("addresses", cfg::Array::of_scalars(cfg::i2c_address_field("address")).min_items(1).unique());
    root.when("mode", "manual", cfg::When().require("addresses"));
    root.when("mode", "scan", cfg::When().forbid("addresses"));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));

    const auto* all_of = find_field(parsed, "allOf");
    ASSERT_NE(all_of, nullptr);
    ASSERT_EQ(all_of->list_value().values_size(), 2);

    const auto& manual_branch = all_of->list_value().values(0).struct_value();
    const auto* if_required = find_path(manual_branch, {"if", "required"});
    ASSERT_NE(if_required, nullptr);
    ASSERT_EQ(if_required->list_value().values_size(), 1);
    EXPECT_EQ(if_required->list_value().values(0).string_value(), "mode");
    EXPECT_EQ(at(manual_branch, {"if", "properties", "mode", "const"}).string_value(), "manual");
    const auto* then_required = find_path(manual_branch, {"then", "required"});
    ASSERT_NE(then_required, nullptr);
    EXPECT_EQ(then_required->list_value().values(0).string_value(), "addresses");

    // Forbidding emits a `false` schema for the key.
    const auto& scan_branch = all_of->list_value().values(1).struct_value();
    const auto* forbidden = find_path(scan_branch, {"then", "properties", "addresses"});
    ASSERT_NE(forbidden, nullptr);
    ASSERT_EQ(forbidden->kind_case(), google::protobuf::Value::kBoolValue);
    EXPECT_FALSE(forbidden->bool_value());
}

TEST(ConfigSchemaEmission, EscapingRoundTrips) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    const std::string tricky = "quote:\" backslash:\\ newline:\n tab:\t bell:\x07 micro:µ";
    cfg::Object root(cfg::Openness::Open);
    root.field(cfg::string_field("note").description(tricky));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    const auto* description = find_path(parsed, {"properties", "note", "description"});
    ASSERT_NE(description, nullptr);
    EXPECT_EQ(description->string_value(), tricky);
}

TEST(ConfigSchemaEmission, EzoLikeFixtureParsesAndCarriesFormMetadata) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    const auto parsed = parse_json(cfg::to_json_schema(make_ezo_like_schema()));

    // Everything the workbench needs to render forms without a catalog:
    EXPECT_EQ(at(parsed, {"properties", "hardware", "properties", "query_delay_us", "default"}).number_value(),
              300000.0);
    EXPECT_NE(find_path(parsed, {"properties", "hardware", "properties", "bus_path", "x-anolis-placeholder"}), nullptr);
    EXPECT_NE(find_path(parsed, {"properties", "devices", "items", "properties", "type", "oneOf"}), nullptr);
    EXPECT_EQ(at(parsed, {"properties", "discovery", "properties", "mode", "const"}).string_value(), "manual");
    const auto* required = find_field(parsed, "required");
    ASSERT_NE(required, nullptr);
    EXPECT_EQ(required->list_value().values_size(), 2);  // hardware, discovery
}

// ---- declaration-time misuse ---------------------------------------------

TEST(ConfigSchemaBuilder, DuplicateMemberKeyThrows) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("name"));
    EXPECT_THROW(root.field(cfg::string_field("name")), std::logic_error);
    EXPECT_THROW(root.child("name", cfg::Object(cfg::Openness::Closed)), std::logic_error);
}

TEST(ConfigSchemaBuilder, KindMismatchedSettersThrow) {
    EXPECT_THROW(cfg::integer_field("x").pattern("^a$"), std::logic_error);
    EXPECT_THROW(cfg::string_field("x").min_int(1), std::logic_error);
    EXPECT_THROW(cfg::integer_field("x").min_number(1.0), std::logic_error);
    EXPECT_THROW(cfg::integer_field("x").non_empty(), std::logic_error);
    EXPECT_THROW(cfg::number_field("x").enum_value("a"), std::logic_error);
    EXPECT_THROW(cfg::integer_field("x").const_value("a"), std::logic_error);
    EXPECT_THROW(cfg::integer_field("x").forbid_value("a"), std::logic_error);
    EXPECT_THROW(cfg::string_field("x").default_int(1), std::logic_error);
    EXPECT_THROW(cfg::integer_field("x").default_string("a"), std::logic_error);
    EXPECT_THROW(cfg::string_field("x").default_bool(true), std::logic_error);
    // I2cAddress bounds are built in.
    EXPECT_THROW(cfg::i2c_address_field("x").min_int(0), std::logic_error);
}

TEST(ConfigSchemaBuilder, NonFiniteNumbersThrow) {
    EXPECT_THROW(cfg::number_field("x").min_number(std::numeric_limits<double>::infinity()), std::logic_error);
    EXPECT_THROW(cfg::number_field("x").max_number(std::numeric_limits<double>::quiet_NaN()), std::logic_error);
    EXPECT_THROW(cfg::number_field("x").default_number(std::numeric_limits<double>::quiet_NaN()), std::logic_error);
}

TEST(ConfigSchemaBuilder, EmptyFieldKeyThrows) { EXPECT_THROW(cfg::string_field(""), std::logic_error); }

TEST(ConfigSchemaBuilder, DuplicateEnumValueThrows) {
    EXPECT_THROW(cfg::string_field("x").enum_value("a").enum_value("a"), std::logic_error);
}

TEST(ConfigSchemaTreeChecks, ConstAndEnumConflictThrows) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("mode").const_value("manual").enum_value("manual"));
    EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
}

TEST(ConfigSchemaTreeChecks, InvertedBoundsThrow) {
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::integer_field("x").min_int(10).max_int(1));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::number_field("x").min_number(10.0).max_number(1.0));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaTreeChecks, DishonestDefaultsThrow) {
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").enum_value("a").default_string("b"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").const_value("a").default_string("b"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("id").pattern("^[a-z]+$").default_string("UPPER"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("id").forbid_value("reserved").default_string("reserved"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::integer_field("x").min_int(10).default_int(1));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::i2c_address_field("address").default_int(0x100));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::number_field("x").max_number(5.0).default_number(9.0));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("x").non_empty().default_string(""));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaTreeChecks, BadPatternThrows) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("x").pattern("(unbalanced"));
    EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
}

TEST(ConfigSchemaTreeChecks, ConditionalReferencesMustBeDeclared) {
    {
        // Unknown discriminator.
        cfg::Object root(cfg::Openness::Closed);
        root.when("mode", "manual", cfg::When().require("addresses"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        // Discriminator that is not a String field.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::integer_field("mode"));
        root.field(cfg::string_field("other"));
        root.when("mode", "1", cfg::When().require("other"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        // require() of an undeclared key.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode"));
        root.when("mode", "manual", cfg::When().require("addresses"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        // forbid() of a required member is a contradiction.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode"));
        root.field(cfg::string_field("path").required());
        root.when("mode", "inert", cfg::When().forbid("path"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaTreeChecks, DependentRequiredReferencesMustBeDeclared) {
    {
        cfg::Object root(cfg::Openness::Closed);
        root.dependent_required("missing", {"other"});
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("present"));
        root.dependent_required("present", {"missing"});
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaTreeChecks, DeprecatedKeyCollidingWithMemberThrows) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("mode"));
    root.deprecated_key("mode", "gone");
    EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
}

TEST(ConfigSchemaTreeChecks, UniqueOutsideArrayItemsThrows) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("name").unique());
    EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
}

TEST(ConfigSchemaTreeChecks, ScalarArrayItemMisuseThrows) {
    {
        cfg::Object root(cfg::Openness::Closed);
        root.array("xs", cfg::Array::of_scalars(cfg::string_field("x").required()));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.array("xs", cfg::Array::of_scalars(cfg::string_field("x").unique()));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaBuilder, ArrayUniqueOnObjectItemsThrows) {
    EXPECT_THROW(cfg::Array::of_objects(cfg::Object(cfg::Openness::Closed)).unique(), std::logic_error);
}

// ---- review-driven regressions (adversarial review of PR 1) ----------------

TEST(ConfigSchemaTreeChecks, DuplicateDependentRequiredThrows) {
    {
        // A repeated key would emit duplicate JSON keys inside dependentRequired
        // and downstream parsers last-win — silently dropping a constraint.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("a"));
        root.field(cfg::string_field("b"));
        root.field(cfg::string_field("c"));
        root.dependent_required("a", {"b"});
        root.dependent_required("a", {"c"});
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("a"));
        root.field(cfg::string_field("b"));
        root.dependent_required("a", {"b", "b"});
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("a"));
        root.dependent_required("a", {"a"});
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaTreeChecks, InvalidUtf8Throws) {
    {
        // A Latin-1 byte would make the emitted document not-JSON downstream.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("note").description("caf\xE9"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").enum_value("ok\xC0\xAF"));  // overlong encoding
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    EXPECT_THROW(cfg::Schema{cfg::Object(cfg::Openness::Open)}.title("bad\xFF"), std::logic_error);
    EXPECT_THROW(
        cfg::config_schema_envelope(cfg::Schema{cfg::Object(cfg::Openness::Open)}, with_provider("bad\xE9name")),
        std::logic_error);
}

TEST(ConfigSchemaTreeChecks, ContradictoryValueConstraintsThrow) {
    {
        // const + forbid of the same value: no document could ever validate.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").const_value("a").forbid_value("a"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        // An enum option that is also forbidden is a dead choice a form would
        // still render.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").enum_value("a").enum_value("b").forbid_value("a"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").const_value("ABC").pattern("^[a-z]+$"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").enum_value("ok").enum_value("BAD").pattern("^[a-z]+$"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").non_empty().const_value(""));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaTreeChecks, WhenEqualsMustBeAmongDeclaredValues) {
    {
        // Typo'd equals vs the discriminator's enum: the branch could never
        // fire — a silent no-op shipping as a schema.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").enum_value("scan").enum_value("manual"));
        root.field(cfg::string_field("extra"));
        root.when("mode", "manul", cfg::When().require("extra"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode").const_value("manual"));
        root.field(cfg::string_field("extra"));
        root.when("mode", "scan", cfg::When().require("extra"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        // A free-form discriminator (no enum/const) accepts any equals.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode"));
        root.field(cfg::string_field("extra"));
        root.when("mode", "anything", cfg::When().require("extra"));
        EXPECT_NO_THROW(cfg::Schema{std::move(root)});
    }
}

TEST(ConfigSchemaTreeChecks, ConflictingWhenBranchesThrow) {
    {
        // require + forbid of the same key within one branch: mode=a could
        // never validate.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode"));
        root.field(cfg::string_field("x"));
        root.when("mode", "a", cfg::When().require("x").forbid("x"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
    {
        // Two branches for the same (key, equals) — reject rather than merge.
        cfg::Object root(cfg::Openness::Closed);
        root.field(cfg::string_field("mode"));
        root.field(cfg::string_field("x"));
        root.field(cfg::string_field("y"));
        root.when("mode", "a", cfg::When().require("x"));
        root.when("mode", "a", cfg::When().forbid("y"));
        EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
    }
}

TEST(ConfigSchemaBuilder, EmptyWhenBranchThrows) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::string_field("mode"));
    EXPECT_THROW(root.when("mode", "a", cfg::When()), std::logic_error);
}

TEST(ConfigSchemaBuilder, DuplicateForbiddenValueThrows) {
    EXPECT_THROW(cfg::string_field("x").forbid_value("a").forbid_value("a"), std::logic_error);
}

TEST(ConfigSchemaBuilder, ExclusiveAndInclusiveBoundConflictThrows) {
    EXPECT_THROW(cfg::number_field("x").min_number(0.0).exclusive_min_number(0.0), std::logic_error);
    EXPECT_THROW(cfg::number_field("x").exclusive_max_number(1.0).max_number(1.0), std::logic_error);
    EXPECT_THROW(cfg::integer_field("x").exclusive_min_number(0.0), std::logic_error);
}

TEST(ConfigSchemaEmission, ExclusiveNumberBounds) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    // motorctl-style bound: max_speed in (0, 10000].
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::number_field("max_speed").exclusive_min_number(0.0).max_number(10000.0));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    EXPECT_EQ(at(parsed, {"properties", "max_speed", "exclusiveMinimum"}).number_value(), 0.0);
    EXPECT_EQ(at(parsed, {"properties", "max_speed", "maximum"}).number_value(), 10000.0);
    EXPECT_EQ(find_path(parsed, {"properties", "max_speed", "minimum"}), nullptr);
}

TEST(ConfigSchemaTreeChecks, DefaultViolatingExclusiveBoundThrows) {
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::number_field("x").exclusive_min_number(0.0).default_number(0.0));
    EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
}

TEST(ConfigSchemaEmission, MaxItems) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    // sim's temp_range: exactly two numbers.
    cfg::Object root(cfg::Openness::Closed);
    root.array("temp_range", cfg::Array::of_scalars(cfg::number_field("bound")).min_items(2).max_items(2));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    EXPECT_EQ(at(parsed, {"properties", "temp_range", "minItems"}).number_value(), 2.0);
    EXPECT_EQ(at(parsed, {"properties", "temp_range", "maxItems"}).number_value(), 2.0);
}

TEST(ConfigSchemaTreeChecks, InvertedItemBoundsThrow) {
    cfg::Object root(cfg::Openness::Closed);
    root.array("xs", cfg::Array::of_scalars(cfg::number_field("x")).min_items(3).max_items(2));
    EXPECT_THROW(cfg::Schema{std::move(root)}, std::logic_error);
}

TEST(ConfigSchemaEmission, ExponentFormDoubleStaysValidJson) {
    ANOLIS_SKIP_ORACLE_UNDER_TSAN();
    cfg::Object root(cfg::Openness::Closed);
    root.field(cfg::number_field("epsilon").min_number(1e-7));
    const auto parsed = parse_json(cfg::to_json_schema(cfg::Schema{std::move(root)}));
    const auto* minimum = find_path(parsed, {"properties", "epsilon", "minimum"});
    ASSERT_NE(minimum, nullptr);
    EXPECT_DOUBLE_EQ(minimum->number_value(), 1e-7);
}

}  // namespace
