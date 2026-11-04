#include "anolis/provider_sdk/call_validation.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "anolis/provider_sdk/handlers.hpp"
#include "anolis/provider_sdk/runtime.hpp"
#include "anolis/provider_sdk/values.hpp"
#include "protocol.pb.h"

// §8.3 call-argument validation: the rules one by one, and through handle_call
// with a runtime whose call would accept anything.

namespace sdk = anolis::provider_sdk;
namespace adpp = anolis::deviceprovider::v1;

namespace {

// set_level(level: double in [0, 1] required, note: string optional, one of a/b)
// and set_count(count: int64 in [-5, 5], limit: uint64 in [1, 10], both optional).
adpp::FunctionSpec level_spec() {
    adpp::FunctionSpec spec;
    spec.set_function_id(1);
    spec.set_name("set_level");
    adpp::ArgSpec* level = spec.add_args();
    *level = sdk::make_arg_spec("level", adpp::VALUE_TYPE_DOUBLE, true);
    level->set_min_double(0.0);
    level->set_max_double(1.0);
    adpp::ArgSpec* note = spec.add_args();
    *note = sdk::make_arg_spec("note", adpp::VALUE_TYPE_STRING, false);
    note->add_allowed_values("a");
    note->add_allowed_values("b");
    return spec;
}

adpp::FunctionSpec count_spec() {
    adpp::FunctionSpec spec;
    spec.set_function_id(2);
    spec.set_name("set_count");
    adpp::ArgSpec* count = spec.add_args();
    *count = sdk::make_arg_spec("count", adpp::VALUE_TYPE_INT64, false);
    count->set_min_int64(-5);
    count->set_max_int64(5);
    adpp::ArgSpec* limit = spec.add_args();
    *limit = sdk::make_arg_spec("limit", adpp::VALUE_TYPE_UINT64, false);
    limit->set_min_uint64(1);
    limit->set_max_uint64(10);
    return spec;
}

sdk::ValueMap args_of(std::initializer_list<std::pair<std::string, adpp::Value>> entries) {
    sdk::ValueMap args;
    for (const auto& [name, value] : entries) {
        args[name] = value;
    }
    return args;
}

std::optional<adpp::Status::Code> code_of(const adpp::FunctionSpec& spec, const sdk::ValueMap& args) {
    const auto err = sdk::validate_call_args(spec, args);
    return err ? std::optional<adpp::Status::Code>(err->code) : std::nullopt;
}

constexpr auto kInvalid = adpp::Status::CODE_INVALID_ARGUMENT;
constexpr auto kOutOfRange = adpp::Status::CODE_OUT_OF_RANGE;

// A runtime that declares set_level and set_count and accepts every call it gets.
class AcceptingRuntime : public sdk::ProviderRuntime {
public:
    int calls = 0;

    sdk::ProviderMetadata metadata() const override { return {"mock", "1.0.0", "v1", {}}; }
    sdk::ReadinessReport readiness() const override { return {}; }
    std::vector<std::string> list_device_ids() const override { return {"dev"}; }
    bool has_device(const std::string& id) const override { return id == "dev"; }
    adpp::Device device_info(const std::string& id) const override {
        adpp::Device d;
        d.set_device_id(id);
        return d;
    }
    adpp::CapabilitySet capabilities(const std::string&) const override {
        adpp::CapabilitySet caps;
        *caps.add_functions() = level_spec();
        *caps.add_functions() = count_spec();
        return caps;
    }
    sdk::AdapterReadResult read(const std::string&, const std::vector<std::string>&) override { return {}; }
    sdk::AdapterCallResult call(const std::string&, uint32_t, const sdk::ValueMap&) override {
        ++calls;
        return sdk::call_ok();
    }
    std::optional<uint32_t> resolve_function_id(const std::string&, const std::string& name) const override {
        if (name == "set_level") return 1U;
        if (name == "set_count") return 2U;
        if (name == "undeclared") return 9U;
        return std::nullopt;
    }
};

adpp::Status::Code call_code(AcceptingRuntime& rt, const std::string& function, const sdk::ValueMap& args) {
    adpp::CallRequest req;
    req.set_device_id("dev");
    req.set_function_name(function);
    *req.mutable_args() = args;
    adpp::Response resp;
    sdk::handlers::handle_call(req, resp, rt);
    return resp.status().code();
}

}  // namespace

TEST(CallValidationTest, TheHandlerRejectsEachKindOfBadArgumentBeforeTheProviderSeesIt) {
    AcceptingRuntime rt;
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(call_code(rt, "set_level", {}), kInvalid);                                               // missing
    EXPECT_EQ(call_code(rt, "set_level", args_of({{"level", sdk::make_int64_val(1)}})), kInvalid);     // wrong type
    EXPECT_EQ(call_code(rt, "set_level", args_of({{"level", sdk::make_double_val(inf)}})), kInvalid);  // non-finite
    EXPECT_EQ(call_code(rt, "set_level", args_of({{"level", sdk::make_double_val(1.5)}})), kOutOfRange);
    EXPECT_EQ(rt.calls, 0);
    EXPECT_EQ(call_code(rt, "set_level", args_of({{"level", sdk::make_double_val(0.5)}})), adpp::Status::CODE_OK);
    EXPECT_EQ(rt.calls, 1);
}

TEST(CallValidationTest, ACallToAnUnknownDeviceIsNotFoundBeforeTheProviderSeesIt) {
    AcceptingRuntime rt;
    adpp::CallRequest req;
    req.set_device_id("nope");
    req.set_function_name("set_level");
    adpp::Response resp;
    sdk::handlers::handle_call(req, resp, rt);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_NOT_FOUND);
    EXPECT_EQ(rt.calls, 0);
}

TEST(CallValidationTest, AFunctionTheDeviceDoesNotDeclareIsLeftToTheProvider) {
    AcceptingRuntime rt;
    EXPECT_EQ(call_code(rt, "undeclared", args_of({{"anything", sdk::make_bool_val(true)}})), adpp::Status::CODE_OK);
    EXPECT_EQ(rt.calls, 1);
}

TEST(CallValidationTest, AnUndeclaredArgumentIsInvalid) {
    const auto err = sdk::validate_call_args(level_spec(), args_of({{"level", sdk::make_double_val(0.5)},
                                                                    {"zeta", sdk::make_bool_val(true)},
                                                                    {"extra", sdk::make_bool_val(true)}}));
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, kInvalid);
    EXPECT_EQ(err->message, "set_level takes no argument 'extra'");  // the first in sorted order
}

TEST(CallValidationTest, AnOptionalArgumentMayBeOmitted) {
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", sdk::make_double_val(0.0)}})), std::nullopt);
    EXPECT_EQ(code_of(count_spec(), {}), std::nullopt);
}

TEST(CallValidationTest, BoundsAreInclusive) {
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", sdk::make_double_val(0.0)}})), std::nullopt);
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", sdk::make_double_val(1.0)}})), std::nullopt);
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", sdk::make_double_val(-0.001)}})), kOutOfRange);
    EXPECT_EQ(code_of(count_spec(), args_of({{"count", sdk::make_int64_val(-5)}})), std::nullopt);
    EXPECT_EQ(code_of(count_spec(), args_of({{"count", sdk::make_int64_val(6)}})), kOutOfRange);
    EXPECT_EQ(code_of(count_spec(), args_of({{"limit", sdk::make_uint64_val(10)}})), std::nullopt);
    EXPECT_EQ(code_of(count_spec(), args_of({{"limit", sdk::make_uint64_val(0)}})), kOutOfRange);
}

TEST(CallValidationTest, NonFiniteIsInvalidBeforeTheBoundsAreChecked) {
    for (const double v : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        EXPECT_EQ(code_of(level_spec(), args_of({{"level", sdk::make_double_val(v)}})), kInvalid) << v;
    }
}

TEST(CallValidationTest, TheTypeTagAndTheFieldMustBothMatchOrBeUnset) {
    adpp::Value untagged;  // a double field with no type tag is accepted
    untagged.set_double_value(0.5);
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", untagged}})), std::nullopt);

    adpp::Value mismatched;  // DOUBLE tag on an int64 field
    mismatched.set_type(adpp::VALUE_TYPE_DOUBLE);
    mismatched.set_int64_value(1);
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", mismatched}})), kInvalid);

    adpp::Value empty;  // no tag and no field
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", empty}})), kInvalid);
}

TEST(CallValidationTest, AllowedValuesConstrainAString) {
    const auto level = sdk::make_double_val(0.5);
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", level}, {"note", sdk::make_string_val("a")}})), std::nullopt);
    EXPECT_EQ(code_of(level_spec(), args_of({{"level", level}, {"note", sdk::make_string_val("c")}})), kInvalid);
}
