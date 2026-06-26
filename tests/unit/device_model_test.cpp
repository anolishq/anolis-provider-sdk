#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "anolis/provider_sdk/device_adapter.hpp"
#include "anolis/provider_sdk/device_spec.hpp"
#include "anolis/provider_sdk/quality.hpp"
#include "anolis/provider_sdk/result.hpp"
#include "anolis/provider_sdk/signals.hpp"
#include "anolis/provider_sdk/values.hpp"

// D.2 device-model framework tests: the neutral result types + call factories,
// the value/arg helper union, the quality hook, the §7.2 default-set helper, and
// the HandleT-templated descriptor. No provider code — exercises the SDK surface
// that the providers migrate onto.

namespace sdk = anolis::provider_sdk;
namespace adpp = anolis::deviceprovider::v1;

// --- result types + call factories ---------------------------------------

TEST(ResultTest, DefaultsAreUnavailableAndNotOk) {
    sdk::AdapterReadResult read;
    EXPECT_FALSE(read.ok);
    EXPECT_EQ(read.error_code, adpp::Status::CODE_UNAVAILABLE);
    EXPECT_TRUE(read.values.empty());

    sdk::AdapterCallResult call;
    EXPECT_FALSE(call.ok);
    EXPECT_EQ(call.error_code, adpp::Status::CODE_UNAVAILABLE);
}

TEST(ResultTest, CallFactoriesMapVocabularyOntoStatusCodes) {
    EXPECT_TRUE(sdk::call_ok().ok);
    EXPECT_EQ(sdk::call_ok().error_code, adpp::Status::CODE_OK);

    const auto bad = sdk::call_invalid_argument("nope");
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.error_code, adpp::Status::CODE_INVALID_ARGUMENT);
    EXPECT_EQ(bad.error_message, "nope");

    EXPECT_EQ(sdk::call_not_found("x").error_code, adpp::Status::CODE_NOT_FOUND);
    EXPECT_EQ(sdk::call_failed_precondition("x").error_code, adpp::Status::CODE_FAILED_PRECONDITION);
    EXPECT_EQ(sdk::call_out_of_range("x").error_code, adpp::Status::CODE_OUT_OF_RANGE);
    EXPECT_EQ(sdk::call_unavailable("x").error_code, adpp::Status::CODE_UNAVAILABLE);
}

// --- value/arg helper union ----------------------------------------------

TEST(ValuesTest, MakersSetTypeAndPayload) {
    EXPECT_EQ(sdk::make_bool_val(true).type(), adpp::VALUE_TYPE_BOOL);
    EXPECT_TRUE(sdk::make_bool_val(true).bool_value());
    EXPECT_EQ(sdk::make_int64_val(-7).type(), adpp::VALUE_TYPE_INT64);
    EXPECT_EQ(sdk::make_int64_val(-7).int64_value(), -7);
    EXPECT_EQ(sdk::make_uint64_val(9u).type(), adpp::VALUE_TYPE_UINT64);
    EXPECT_EQ(sdk::make_uint64_val(9u).uint64_value(), 9u);
    EXPECT_EQ(sdk::make_double_val(1.5).type(), adpp::VALUE_TYPE_DOUBLE);
    EXPECT_DOUBLE_EQ(sdk::make_double_val(1.5).double_value(), 1.5);
    EXPECT_EQ(sdk::make_string_val("hi").type(), adpp::VALUE_TYPE_STRING);
    EXPECT_EQ(sdk::make_string_val("hi").string_value(), "hi");
}

TEST(ValuesTest, GetArgRoundTripsEachType) {
    sdk::ValueMap args;
    args["b"] = sdk::make_bool_val(true);
    args["i"] = sdk::make_int64_val(-3);
    args["u"] = sdk::make_uint64_val(42u);  // the bread-only type sim couldn't emit
    args["d"] = sdk::make_double_val(2.25);
    args["s"] = sdk::make_string_val("v");

    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;
    std::string s;
    EXPECT_TRUE(sdk::get_arg_bool(args, "b", b));
    EXPECT_TRUE(b);
    EXPECT_TRUE(sdk::get_arg_int64(args, "i", i));
    EXPECT_EQ(i, -3);
    EXPECT_TRUE(sdk::get_arg_uint64(args, "u", u));
    EXPECT_EQ(u, 42u);
    EXPECT_TRUE(sdk::get_arg_double(args, "d", d));
    EXPECT_DOUBLE_EQ(d, 2.25);
    EXPECT_TRUE(sdk::get_arg_string(args, "s", s));
    EXPECT_EQ(s, "v");
}

TEST(ValuesTest, GetArgRejectsMissingKeyAndTypeMismatch) {
    sdk::ValueMap args;
    args["b"] = sdk::make_bool_val(true);

    bool b = false;
    int64_t i = 0;
    EXPECT_FALSE(sdk::get_arg_bool(args, "absent", b));  // missing key
    EXPECT_FALSE(sdk::get_arg_int64(args, "b", i));      // wrong type
}

TEST(ValuesTest, MakeSignalValueStampsIdAndQualityOk) {
    const auto sv = sdk::make_signal_value("temp", sdk::make_double_val(21.0));
    EXPECT_EQ(sv.signal_id(), "temp");
    EXPECT_EQ(sv.value().type(), adpp::VALUE_TYPE_DOUBLE);
    EXPECT_DOUBLE_EQ(sv.value().double_value(), 21.0);
    EXPECT_EQ(sv.quality(), adpp::SignalValue::QUALITY_OK);
    EXPECT_TRUE(sv.has_timestamp());
}

TEST(ValuesTest, CapabilityBuildersSetFields) {
    const auto arg = sdk::make_arg_spec("setpoint", adpp::VALUE_TYPE_DOUBLE, true, "target", "C");
    EXPECT_EQ(arg.name(), "setpoint");
    EXPECT_EQ(arg.type(), adpp::VALUE_TYPE_DOUBLE);
    EXPECT_TRUE(arg.required());
    EXPECT_EQ(arg.unit(), "C");

    const auto policy = sdk::make_function_policy(adpp::FunctionPolicy::CATEGORY_ACTUATE, true, false, 100);
    EXPECT_EQ(policy.category(), adpp::FunctionPolicy::CATEGORY_ACTUATE);
    EXPECT_TRUE(policy.requires_lease());
    EXPECT_FALSE(policy.is_idempotent());
    EXPECT_EQ(policy.min_interval_ms(), 100);
}

// --- quality hook ---------------------------------------------------------

TEST(QualityTest, TruthTableCoversEveryBranch) {
    using Q = adpp::SignalValue;
    // unavailable -> UNKNOWN (regardless of the rest)
    EXPECT_EQ(sdk::quality_from(false, true, true, 0, 1000), Q::QUALITY_UNKNOWN);
    // available but last read failed -> FAULT
    EXPECT_EQ(sdk::quality_from(true, true, false, 0, 1000), Q::QUALITY_FAULT);
    // available, ok, but too old -> STALE
    EXPECT_EQ(sdk::quality_from(true, true, true, 2000, 1000), Q::QUALITY_STALE);
    // available, ok, fresh, has value -> OK
    EXPECT_EQ(sdk::quality_from(true, true, true, 10, 1000), Q::QUALITY_OK);
    // available, ok, fresh, but no value -> UNKNOWN
    EXPECT_EQ(sdk::quality_from(true, false, true, 10, 1000), Q::QUALITY_UNKNOWN);
}

// --- §7.2 default-set helper ---------------------------------------------

TEST(SignalsTest, DefaultIdsFromKeepsOnlyDefaultsInOrder) {
    static constexpr std::array<sdk::SignalDefinition, 3> kDefs{{
        {"ph", "pH", "acidity", "", true},
        {"slope", "Slope", "calibration slope", "%", false},
        {"temp", "Temperature", "compensated", "C", true},
    }};
    const auto ids = sdk::default_ids_from(std::span<const sdk::SignalDefinition>(kDefs));
    EXPECT_EQ(ids, (std::vector<std::string>{"ph", "temp"}));
}

// --- HandleT-templated descriptor ----------------------------------------

namespace {

// A fake hardware handle to prove a non-trivial HandleT threads through.
struct FakeHandle {
    int reads = 0;
};

sdk::AdapterReadResult fake_read(FakeHandle& h, const sdk::DeviceSpec& device,
                                 const std::vector<std::string>& signal_ids) {
    ++h.reads;
    sdk::AdapterReadResult r;
    r.ok = true;
    r.error_code = adpp::Status::CODE_OK;
    r.values.push_back(sdk::make_signal_value(device.id + "/" + (signal_ids.empty() ? "default" : signal_ids.front()),
                                              sdk::make_double_val(1.0)));
    return r;
}

sdk::AdapterCallResult fake_call(FakeHandle&, const sdk::DeviceSpec&, uint32_t, const sdk::ValueMap&) {
    return sdk::call_ok();
}

adpp::CapabilitySet fake_caps() { return {}; }

// A session-less (sim-style) read over std::monostate.
sdk::AdapterReadResult monostate_read(std::monostate&, const sdk::DeviceSpec&, const std::vector<std::string>&) {
    return {true, adpp::Status::CODE_OK, "", {}};
}

}  // namespace

TEST(DeviceAdapterTest, ThreadsHandleAndDefaultsOptionalMembersNull) {
    sdk::DeviceAdapter<FakeHandle> adapter{};
    adapter.read_signals = &fake_read;
    adapter.call = &fake_call;

    // Optional members default to null (the ezo/bread case — computed in core).
    EXPECT_EQ(adapter.get_capabilities, nullptr);
    EXPECT_EQ(adapter.get_device_info, nullptr);

    FakeHandle handle;
    const sdk::DeviceSpec spec{"dev0", "Device 0", "fake.kind"};
    const auto read = adapter.read_signals(handle, spec, {});
    EXPECT_TRUE(read.ok);
    EXPECT_EQ(handle.reads, 1);
    ASSERT_EQ(read.values.size(), 1u);
    EXPECT_EQ(read.values.front().signal_id(), "dev0/default");

    EXPECT_TRUE(adapter.call(handle, spec, 0, {}).ok);
}

TEST(DeviceAdapterTest, OptionalMembersDiscriminateProviderShape) {
    // sim-style: owns capabilities on the descriptor.
    sdk::DeviceAdapter<std::monostate> sim_like{};
    sim_like.read_signals = &monostate_read;
    sim_like.get_capabilities = &fake_caps;
    EXPECT_NE(sim_like.get_capabilities, nullptr);

    // ezo/bread-style: leaves it null (capabilities built in core).
    sdk::DeviceAdapter<FakeHandle> ezo_like{};
    ezo_like.read_signals = &fake_read;
    EXPECT_EQ(ezo_like.get_capabilities, nullptr);
}
