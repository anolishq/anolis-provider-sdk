#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "anolis/provider_sdk/handlers.hpp"
#include "anolis/provider_sdk/runtime.hpp"
#include "anolis/provider_sdk/values.hpp"
#include "protocol.pb.h"

// D.3c device-model handler tests: list/describe/read/call/get_health policy
// (§7.2 default-set passthrough, §7.4 unknown-signal NOT_FOUND, §7.3 staleness,
// §6.2 function resolution, the `accepted` call shim) via a richer mock runtime.

namespace sdk = anolis::provider_sdk;
namespace adpp = anolis::deviceprovider::v1;

namespace {

class DeviceMock : public sdk::ProviderRuntime {
public:
    bool read_ok = true;

    sdk::ProviderMetadata metadata() const override { return {"mock", "1.0.0", "v1", {}}; }
    sdk::ReadinessReport readiness() const override {
        sdk::ReadinessReport r;
        r.configured_device_count = 2;
        r.successful_device_ids = {"temp"};
        r.failed_devices = {{"flaky", "kind", "init timeout"}};
        r.provider_impl = "mock";
        r.startup_policy = "degraded";
        return r;
    }
    std::vector<std::string> list_device_ids() const override { return {"temp"}; }
    bool has_device(const std::string& id) const override { return id == "temp"; }
    adpp::Device device_info(const std::string& id) const override {
        adpp::Device d;
        d.set_device_id(id);
        return d;
    }
    adpp::CapabilitySet capabilities(const std::string&) const override {
        adpp::CapabilitySet caps;
        caps.add_signals()->set_signal_id("temp_c");
        caps.add_signals()->set_signal_id("temp_f");
        return caps;
    }
    sdk::AdapterReadResult read(const std::string&, const std::vector<std::string>& ids) override {
        sdk::AdapterReadResult r;
        if (!read_ok) {
            r.ok = false;
            r.error_code = adpp::Status::CODE_UNAVAILABLE;
            r.error_message = "bus down";
            return r;
        }
        r.ok = true;
        r.error_code = adpp::Status::CODE_OK;
        const std::vector<std::string> to_emit = ids.empty() ? std::vector<std::string>{"temp_c"} : ids;
        for (const auto& id : to_emit) {
            r.values.push_back(sdk::make_signal_value(id, sdk::make_double_val(21.0)));
        }
        return r;
    }
    sdk::AdapterCallResult call(const std::string&, uint32_t function_id, const sdk::ValueMap&) override {
        if (function_id != 1) {
            return sdk::call_not_found("no such function_id");
        }
        return sdk::call_ok();
    }
    std::optional<uint32_t> resolve_function_id(const std::string&, const std::string& name) const override {
        if (name == "set_target") return 1u;
        return std::nullopt;
    }
};

}  // namespace

TEST(DeviceHandlersTest, ListDevicesWithAndWithoutHealth) {
    DeviceMock rt;
    adpp::Response resp;
    adpp::ListDevicesRequest req;
    sdk::handlers::handle_list_devices(req, resp, rt);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_OK);
    ASSERT_EQ(resp.list_devices().devices_size(), 1);
    EXPECT_EQ(resp.list_devices().device_health_size(), 0);

    adpp::Response resp_h;
    req.set_include_health(true);
    sdk::handlers::handle_list_devices(req, resp_h, rt);
    // Health covers exactly the LISTED devices (one entry for the live "temp").
    // The startup-failed "flaky" is NOT live, so it must NOT appear here — the
    // harness rejects health for devices not in the inventory.
    ASSERT_EQ(resp_h.list_devices().device_health_size(), 1);
    EXPECT_EQ(resp_h.list_devices().device_health(0).device_id(), "temp");
    EXPECT_EQ(resp_h.list_devices().device_health(0).state(), adpp::DeviceHealth::STATE_OK);
}

TEST(DeviceHandlersTest, DescribeDeviceValidatesAndReturnsCaps) {
    DeviceMock rt;
    adpp::Response empty_resp;
    adpp::DescribeDeviceRequest empty;
    sdk::handlers::handle_describe_device(empty, empty_resp, rt);
    EXPECT_EQ(empty_resp.status().code(), adpp::Status::CODE_INVALID_ARGUMENT);

    adpp::Response nf_resp;
    adpp::DescribeDeviceRequest nf;
    nf.set_device_id("ghost");
    sdk::handlers::handle_describe_device(nf, nf_resp, rt);
    EXPECT_EQ(nf_resp.status().code(), adpp::Status::CODE_NOT_FOUND);

    adpp::Response ok_resp;
    adpp::DescribeDeviceRequest ok;
    ok.set_device_id("temp");
    sdk::handlers::handle_describe_device(ok, ok_resp, rt);
    EXPECT_EQ(ok_resp.status().code(), adpp::Status::CODE_OK);
    EXPECT_EQ(ok_resp.describe_device().device().device_id(), "temp");
    EXPECT_EQ(ok_resp.describe_device().capabilities().signals_size(), 2);
}

TEST(DeviceHandlersTest, ReadSignalsPolicy) {
    DeviceMock rt;

    // empty device_id -> INVALID_ARGUMENT
    {
        adpp::Response resp;
        adpp::ReadSignalsRequest req;
        sdk::handlers::handle_read_signals(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_INVALID_ARGUMENT);
    }
    // unknown device -> NOT_FOUND
    {
        adpp::Response resp;
        adpp::ReadSignalsRequest req;
        req.set_device_id("ghost");
        sdk::handlers::handle_read_signals(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_NOT_FOUND);
    }
    // unknown signal_id -> NOT_FOUND for the whole read (§7.4)
    {
        adpp::Response resp;
        adpp::ReadSignalsRequest req;
        req.set_device_id("temp");
        req.add_signal_ids("temp_c");
        req.add_signal_ids("bogus");
        sdk::handlers::handle_read_signals(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_NOT_FOUND);
    }
    // empty signal_ids -> default set (§7.2)
    {
        adpp::Response resp;
        adpp::ReadSignalsRequest req;
        req.set_device_id("temp");
        sdk::handlers::handle_read_signals(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_OK);
        ASSERT_EQ(resp.read_signals().values_size(), 1);
        EXPECT_EQ(resp.read_signals().values(0).signal_id(), "temp_c");
    }
    // adapter read failure maps the neutral error through
    {
        rt.read_ok = false;
        adpp::Response resp;
        adpp::ReadSignalsRequest req;
        req.set_device_id("temp");
        sdk::handlers::handle_read_signals(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_UNAVAILABLE);
        EXPECT_EQ(resp.status().message(), "bus down");
    }
}

TEST(DeviceHandlersTest, ApplyMinTimestampFlagsStaleValues) {
    adpp::ReadSignalsResponse out;
    auto* v = out.add_values();
    v->set_signal_id("temp_c");
    v->mutable_timestamp()->set_seconds(1000);  // old
    v->set_quality(adpp::SignalValue::QUALITY_OK);

    adpp::ReadSignalsRequest req;
    req.mutable_min_timestamp()->set_seconds(2000);  // require fresher than the value
    sdk::handlers::apply_min_timestamp(req, out);
    EXPECT_EQ(out.values(0).quality(), adpp::SignalValue::QUALITY_STALE);

    // a value at/after min_timestamp is left untouched
    adpp::ReadSignalsResponse fresh;
    auto* fv = fresh.add_values();
    fv->mutable_timestamp()->set_seconds(3000);
    fv->set_quality(adpp::SignalValue::QUALITY_OK);
    sdk::handlers::apply_min_timestamp(req, fresh);
    EXPECT_EQ(fresh.values(0).quality(), adpp::SignalValue::QUALITY_OK);
}

TEST(DeviceHandlersTest, ApplyMinTimestampDoesNotMaskFaultOrUnknown) {
    // #10: an unmet freshness hint must not downgrade FAULT/UNKNOWN to STALE — STALE
    // ("usable but old") would understate a broken/unavailable value.
    adpp::ReadSignalsRequest req;
    req.mutable_min_timestamp()->set_seconds(2000);  // require fresher than the values

    for (const auto quality : {adpp::SignalValue::QUALITY_FAULT, adpp::SignalValue::QUALITY_UNKNOWN,
                               adpp::SignalValue::QUALITY_UNSPECIFIED}) {
        adpp::ReadSignalsResponse out;
        auto* v = out.add_values();
        v->mutable_timestamp()->set_seconds(1000);  // old -> would be flagged if OK
        v->set_quality(quality);
        sdk::handlers::apply_min_timestamp(req, out);
        EXPECT_EQ(out.values(0).quality(), quality) << "quality " << quality << " must survive the freshness gate";
    }
}

TEST(DeviceHandlersTest, CallPolicyAndAcceptedShim) {
    DeviceMock rt;

    // missing function -> INVALID_ARGUMENT
    {
        adpp::Response resp;
        adpp::CallRequest req;
        req.set_device_id("temp");
        sdk::handlers::handle_call(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_INVALID_ARGUMENT);
    }
    // unknown function_name -> NOT_FOUND
    {
        adpp::Response resp;
        adpp::CallRequest req;
        req.set_device_id("temp");
        req.set_function_name("nope");
        sdk::handlers::handle_call(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_NOT_FOUND);
    }
    // resolve by name -> ok + accepted shim
    {
        adpp::Response resp;
        adpp::CallRequest req;
        req.set_device_id("temp");
        req.set_function_name("set_target");
        sdk::handlers::handle_call(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_OK);
        EXPECT_TRUE(resp.call().results().at("accepted").bool_value());
    }
    // by function_id directly -> ok
    {
        adpp::Response resp;
        adpp::CallRequest req;
        req.set_device_id("temp");
        req.set_function_id(1);
        sdk::handlers::handle_call(req, resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_OK);
    }
}

TEST(DeviceHandlersTest, ReturnedResultsReplaceAccepted) {
    class ResultsMock : public DeviceMock {
    public:
        sdk::ValueMap returned;
        sdk::AdapterCallResult call(const std::string&, uint32_t, const sdk::ValueMap&) override {
            return sdk::call_ok(returned);
        }
    };
    ResultsMock rt;
    adpp::CallRequest req;
    req.set_device_id("temp");
    req.set_function_id(1);

    // No results returned: the constant `accepted`, as before.
    adpp::Response resp;
    sdk::handlers::handle_call(req, resp, rt);
    ASSERT_EQ(resp.status().code(), adpp::Status::CODE_OK);
    ASSERT_EQ(resp.call().results().size(), 1);
    EXPECT_TRUE(resp.call().results().at("accepted").bool_value());

    // Results returned: exactly those, so a function's declared results carry no
    // undeclared key.
    rt.returned["energy"] = sdk::make_double_val(500.0);
    adpp::Response resp2;
    sdk::handlers::handle_call(req, resp2, rt);
    ASSERT_EQ(resp2.status().code(), adpp::Status::CODE_OK);
    ASSERT_EQ(resp2.call().results().size(), 1);
    ASSERT_EQ(resp2.call().results().count("energy"), 1U);
    EXPECT_DOUBLE_EQ(resp2.call().results().at("energy").double_value(), 500.0);
}

//=============================================================================
// Call deadlines (semantics §8.4)
//=============================================================================

namespace {

google::protobuf::Timestamp timestamp_at(std::chrono::system_clock::time_point tp) {
    const auto since = tp.time_since_epoch();
    const auto secs = std::chrono::floor<std::chrono::seconds>(since);
    google::protobuf::Timestamp ts;
    ts.set_seconds(secs.count());
    ts.set_nanos(static_cast<int32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since - secs).count()));
    return ts;
}

// Counts the calls that reach the provider, and runs `during` inside each one.
struct CallSpy : DeviceMock {
    int calls = 0;
    std::function<void()> during;
    sdk::AdapterCallResult call(const std::string& device_id, uint32_t function_id,
                                const sdk::ValueMap& args) override {
        ++calls;
        if (during) {
            during();
        }
        return DeviceMock::call(device_id, function_id, args);
    }
};

adpp::CallRequest call_with_deadline(const google::protobuf::Timestamp& deadline) {
    adpp::CallRequest req;
    req.set_device_id("temp");
    req.set_function_id(1);
    *req.mutable_deadline() = deadline;
    return req;
}

}  // namespace

TEST(DeviceHandlersTest, HelloAdvertisesDeadlineSupport) {
    DeviceMock rt;
    adpp::HelloRequest req;
    req.set_protocol_version("v1");
    adpp::Response resp;
    sdk::handlers::handle_hello(req, resp, rt);
    ASSERT_EQ(resp.status().code(), adpp::Status::CODE_OK);
    ASSERT_TRUE(resp.hello().metadata().contains("supports_deadlines"));
    EXPECT_EQ(resp.hello().metadata().at("supports_deadlines"), "true");
}

TEST(DeviceHandlersTest, ExpiredDeadlineIsRefusedBeforeTheProviderRuns) {
    CallSpy rt;
    for (const auto& deadline : {timestamp_at(std::chrono::system_clock::now() - std::chrono::hours(1)),
                                 timestamp_at(std::chrono::system_clock::time_point{std::chrono::seconds(1)})}) {
        adpp::Response resp;
        sdk::handlers::handle_call(call_with_deadline(deadline), resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_DEADLINE_EXCEEDED) << deadline.seconds();
        EXPECT_FALSE(resp.has_call());
    }
    EXPECT_EQ(rt.calls, 0) << "an expired call must not reach ProviderRuntime::call";
}

TEST(DeviceHandlersTest, MalformedDeadlineIsInvalidArgument) {
    CallSpy rt;
    const auto future = timestamp_at(std::chrono::system_clock::now() + std::chrono::hours(1));
    std::vector<google::protobuf::Timestamp> malformed;
    for (const int32_t nanos : {-1, 1'000'000'000, 2'000'000'000}) {
        auto ts = future;
        ts.set_nanos(nanos);
        malformed.push_back(ts);
    }
    for (const int64_t seconds : {int64_t{-62135596801}, int64_t{253402300800}}) {  // outside 0001..9999
        google::protobuf::Timestamp ts;
        ts.set_seconds(seconds);
        malformed.push_back(ts);
    }
    for (const auto& deadline : malformed) {
        adpp::Response resp;
        sdk::handlers::handle_call(call_with_deadline(deadline), resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_INVALID_ARGUMENT)
            << deadline.seconds() << "s " << deadline.nanos() << "ns";
    }
    EXPECT_EQ(rt.calls, 0);
}

TEST(DeviceHandlersTest, FutureDeadlineRunsTheCall) {
    CallSpy rt;
    google::protobuf::Timestamp last;  // the latest valid Timestamp, 9999-12-31T23:59:59.999999999Z
    last.set_seconds(253402300799);
    last.set_nanos(999'999'999);
    for (const auto& deadline : {timestamp_at(std::chrono::system_clock::now() + std::chrono::hours(1)), last}) {
        adpp::Response resp;
        sdk::handlers::handle_call(call_with_deadline(deadline), resp, rt);
        EXPECT_EQ(resp.status().code(), adpp::Status::CODE_OK) << resp.status().message();
        EXPECT_TRUE(resp.call().results().at("accepted").bool_value());
    }
    EXPECT_EQ(rt.calls, 2);
}

TEST(DeviceHandlersTest, DeadlinePassingDuringTheCallIsReported) {
    CallSpy rt;
    // Far enough ahead that the pre-call check passes even under TSan, then
    // the "call" outlives it.
    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(500);
    rt.during = [deadline] {
        while (std::chrono::system_clock::now() <= deadline + std::chrono::milliseconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    adpp::Response resp;
    sdk::handlers::handle_call(call_with_deadline(timestamp_at(deadline)), resp, rt);
    EXPECT_EQ(rt.calls, 1);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_DEADLINE_EXCEEDED);
    ASSERT_TRUE(resp.status().details().contains("side_effects"));
    EXPECT_EQ(resp.status().details().at("side_effects"), "possible");
    EXPECT_FALSE(resp.has_call()) << "a call that missed its deadline must not also report results";
}

namespace {

// Function 1 "f" declares one required int64 argument "n".
struct DeclaringSpy : CallSpy {
    adpp::CapabilitySet capabilities(const std::string&) const override {
        adpp::CapabilitySet caps;
        auto* f = caps.add_functions();
        f->set_function_id(1);
        f->set_name("f");
        auto* a = f->add_args();
        a->set_name("n");
        a->set_type(adpp::VALUE_TYPE_INT64);
        a->set_required(true);
        return caps;
    }
};

// Counts the call, then fails it.
struct FailingSpy : CallSpy {
    sdk::AdapterCallResult call(const std::string& device_id, uint32_t function_id,
                                const sdk::ValueMap& args) override {
        CallSpy::call(device_id, function_id, args);
        return sdk::call_unavailable("bus down");
    }
};

}  // namespace

TEST(DeviceHandlersTest, ArgumentErrorsComeBeforeTheDeadline) {
    // The SDK's §8.3 checks run first, as in the Python SDK.
    DeclaringSpy rt;
    adpp::Response resp;  // required "n" missing
    sdk::handlers::handle_call(
        call_with_deadline(timestamp_at(std::chrono::system_clock::now() - std::chrono::hours(1))), resp, rt);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_INVALID_ARGUMENT) << resp.status().message();
    EXPECT_EQ(rt.calls, 0);
}

TEST(DeviceHandlersTest, AnUndeclaredFunctionIdMeetsTheDeadlineCheckFirst) {
    // The SDK leaves an undeclared function_id to the provider (§8.3 note in
    // handle_call), so it cannot answer NOT_FOUND before the deadline check as
    // the Python SDK does: an expired or malformed deadline is reported, and
    // the provider is still not called.
    CallSpy rt;
    auto expired = call_with_deadline(timestamp_at(std::chrono::system_clock::now() - std::chrono::hours(1)));
    expired.set_function_id(99);
    adpp::Response resp;
    sdk::handlers::handle_call(expired, resp, rt);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_DEADLINE_EXCEEDED);

    google::protobuf::Timestamp bad;
    bad.set_nanos(-1);
    auto malformed = call_with_deadline(bad);
    malformed.set_function_id(99);
    adpp::Response resp2;
    sdk::handlers::handle_call(malformed, resp2, rt);
    EXPECT_EQ(resp2.status().code(), adpp::Status::CODE_INVALID_ARGUMENT);
    EXPECT_EQ(rt.calls, 0);
}

TEST(DeviceHandlersTest, TheLowestValidTimestampIsExpiredNotMalformed) {
    CallSpy rt;
    google::protobuf::Timestamp first;  // 0001-01-01T00:00:00Z
    first.set_seconds(-62135596800);
    adpp::Response resp;
    sdk::handlers::handle_call(call_with_deadline(first), resp, rt);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_DEADLINE_EXCEEDED);
    EXPECT_EQ(rt.calls, 0);
}

TEST(DeviceHandlersTest, AFailedCallPastItsDeadlineKeepsItsOwnError) {
    FailingSpy rt;
    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(500);
    rt.during = [deadline] {
        while (std::chrono::system_clock::now() <= deadline + std::chrono::milliseconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    adpp::Response resp;
    sdk::handlers::handle_call(call_with_deadline(timestamp_at(deadline)), resp, rt);
    EXPECT_EQ(rt.calls, 1);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_UNAVAILABLE);
    EXPECT_EQ(resp.status().message(), "bus down");
    EXPECT_FALSE(resp.status().details().contains("side_effects"));
}

TEST(DeviceHandlersTest, LiveDeviceNotInStartupReportStillGetsHealth) {
    // Regression (caught by sim conformance): a live device absent from the
    // startup report — e.g. a synthetic control channel like sim's chaos_control —
    // must still get a health entry. list_devices(include_health) MUST cover every
    // listed device, and health must not reference unlisted devices.
    struct LiveOnlyMock : DeviceMock {
        std::vector<std::string> list_device_ids() const override { return {"temp", "control"}; }
    } rt;  // readiness() (inherited) knows only "temp"; "control" is live-only
    adpp::Response resp;
    adpp::ListDevicesRequest req;
    req.set_include_health(true);
    sdk::handlers::handle_list_devices(req, resp, rt);

    std::set<std::string> health_ids;
    for (const auto& dh : resp.list_devices().device_health()) {
        health_ids.insert(dh.device_id());
    }
    EXPECT_EQ(health_ids, (std::set<std::string>{"temp", "control"}));
}

TEST(DeviceHandlersTest, GetHealthIsDegradedWithAFailedDevice) {
    DeviceMock rt;
    adpp::Response resp;
    adpp::GetHealthRequest req;
    sdk::handlers::handle_get_health(req, resp, rt);
    EXPECT_EQ(resp.status().code(), adpp::Status::CODE_OK);
    EXPECT_EQ(resp.get_health().provider().state(), adpp::ProviderHealth::STATE_DEGRADED);
    EXPECT_EQ(resp.get_health().devices_size(), 2);  // temp OK + flaky FAULT
}

// SDK#9: a mock that enriches per-device health for every id (live AND failed).
namespace {
struct EnrichMock : DeviceMock {
    sdk::DeviceHealthExtra device_health(const std::string& id) const override {
        sdk::DeviceHealthExtra e;
        e.metrics["impl"] = "mock";
        e.metrics["addr"] = id;  // per-device so we can tell entries apart
        google::protobuf::Timestamp ts;
        ts.set_seconds(1700000000);
        e.last_seen = ts;
        return e;
    }
};
}  // namespace

TEST(DeviceHandlersTest, DeviceHealthEnrichmentMergesMetricsAndLastSeen) {
    EnrichMock rt;
    adpp::ListDevicesRequest req;
    req.set_include_health(true);
    adpp::Response resp;
    sdk::handlers::handle_list_devices(req, resp, rt);
    ASSERT_EQ(resp.list_devices().device_health_size(), 1);
    const auto& dh = resp.list_devices().device_health(0);
    EXPECT_EQ(dh.device_id(), "temp");
    ASSERT_TRUE(dh.metrics().contains("impl"));
    EXPECT_EQ(dh.metrics().at("impl"), "mock");
    EXPECT_EQ(dh.metrics().at("addr"), "temp");
    EXPECT_TRUE(dh.has_last_seen());
    EXPECT_EQ(dh.last_seen().seconds(), 1700000000);
}

TEST(DeviceHandlersTest, DeviceHealthNoOverrideLeavesWireUnchanged) {
    // A provider that does NOT override device_health (the default) must emit no
    // metrics and an unset last_seen — exact pre-SDK#9 wire output (backward-compat).
    DeviceMock rt;
    adpp::ListDevicesRequest req;
    req.set_include_health(true);
    adpp::Response resp;
    sdk::handlers::handle_list_devices(req, resp, rt);
    ASSERT_EQ(resp.list_devices().device_health_size(), 1);
    const auto& dh = resp.list_devices().device_health(0);
    EXPECT_EQ(dh.metrics_size(), 0);
    EXPECT_FALSE(dh.has_last_seen());
}

TEST(DeviceHandlersTest, DeviceHealthEmptyExtraDoesNotMaterializeFields) {
    // An override that returns an empty struct must be indistinguishable on the
    // wire from no override — the presence guards must not materialize metrics
    // (field 5) or stamp an epoch-0 last_seen (field 4).
    struct EmptyExtraMock : DeviceMock {
        sdk::DeviceHealthExtra device_health(const std::string&) const override { return {}; }
    } rt;
    adpp::ListDevicesRequest req;
    req.set_include_health(true);
    adpp::Response resp;
    sdk::handlers::handle_list_devices(req, resp, rt);
    ASSERT_EQ(resp.list_devices().device_health_size(), 1);
    const auto& dh = resp.list_devices().device_health(0);
    EXPECT_EQ(dh.metrics_size(), 0);
    EXPECT_FALSE(dh.has_last_seen());
}

TEST(DeviceHandlersTest, DeviceHealthEnrichmentReachesFailedDeviceOnGetHealthOnly) {
    // The enrichment hook fires for the failed/missing "flaky" id on get_health
    // (live ∪ failed), but "flaky" must NOT appear on list_devices(include_health)
    // (the conformance MUST: health ⊆ live devices). Asymmetry is protocol-mandated.
    EnrichMock rt;

    adpp::GetHealthRequest gh_req;
    adpp::Response gh_resp;
    sdk::handlers::handle_get_health(gh_req, gh_resp, rt);
    ASSERT_EQ(gh_resp.get_health().devices_size(), 2);  // temp (live) + flaky (failed)
    std::set<std::string> enriched;
    for (const auto& dh : gh_resp.get_health().devices()) {
        ASSERT_TRUE(dh.metrics().contains("addr")) << dh.device_id();
        EXPECT_EQ(dh.metrics().at("addr"), dh.device_id());
        EXPECT_TRUE(dh.has_last_seen());
        enriched.insert(dh.device_id());
    }
    EXPECT_EQ(enriched, (std::set<std::string>{"temp", "flaky"}));

    adpp::ListDevicesRequest ld_req;
    ld_req.set_include_health(true);
    adpp::Response ld_resp;
    sdk::handlers::handle_list_devices(ld_req, ld_resp, rt);
    ASSERT_EQ(ld_resp.list_devices().device_health_size(), 1);  // live "temp" only
    EXPECT_EQ(ld_resp.list_devices().device_health(0).device_id(), "temp");
}

//=============================================================================
// Per-device state override (ezo#87) + provider-metrics hook (ezo#88)
//=============================================================================

namespace {
// Overrides a live device's state to FAULT and provides provider metrics + an
// escalated provider state.
struct StateOverrideMock : DeviceMock {
    sdk::DeviceHealthExtra device_health(const std::string& id) const override {
        sdk::DeviceHealthExtra e;
        if (id == "temp") {
            e.state = adpp::DeviceHealth::STATE_FAULT;
            e.message = "latest read failed";
        }
        return e;
    }
    sdk::ProviderHealthExtra provider_health() const override {
        sdk::ProviderHealthExtra e;
        e.metrics["excluded_devices"] = "1";
        e.metrics["impl"] = "evil";  // must not overwrite the fixed lifecycle key
        return e;
    }
};

// Healthy startup (no failed devices) so the provider state is OK unless a
// provider override escalates it.
struct HealthyEscalateMock : DeviceMock {
    sdk::ReadinessReport readiness() const override {
        sdk::ReadinessReport r;
        r.configured_device_count = 1;
        r.successful_device_ids = {"temp"};
        r.provider_impl = "mock";
        r.startup_policy = "strict";
        return r;
    }
    sdk::ProviderHealthExtra provider_health() const override {
        sdk::ProviderHealthExtra e;
        e.state = adpp::ProviderHealth::STATE_DEGRADED;
        e.message = "i2c executor stopped";
        return e;
    }
};
}  // namespace

TEST(DeviceHandlersTest, DeviceStateOverrideReplacesReadinessState) {
    StateOverrideMock rt;
    adpp::GetHealthRequest req;
    adpp::Response resp;
    sdk::handlers::handle_get_health(req, resp, rt);
    bool saw_temp = false;
    for (const auto& dh : resp.get_health().devices()) {
        if (dh.device_id() == "temp") {
            saw_temp = true;
            EXPECT_EQ(dh.state(), adpp::DeviceHealth::STATE_FAULT);
            EXPECT_EQ(dh.message(), "latest read failed");
        }
    }
    EXPECT_TRUE(saw_temp);
}

TEST(DeviceHandlersTest, DeviceStateNoOverrideKeepsReadinessDerivedState) {
    DeviceMock rt;  // no override
    adpp::GetHealthRequest req;
    adpp::Response resp;
    sdk::handlers::handle_get_health(req, resp, rt);
    ASSERT_EQ(resp.get_health().devices_size(), 2);  // temp (live) + flaky (failed)
    for (const auto& dh : resp.get_health().devices()) {
        if (dh.device_id() == "temp") {
            EXPECT_EQ(dh.state(), adpp::DeviceHealth::STATE_OK);
            EXPECT_EQ(dh.message(), "ok");  // message is part of the backward-compat surface
        } else if (dh.device_id() == "flaky") {
            EXPECT_EQ(dh.state(), adpp::DeviceHealth::STATE_UNREACHABLE);
            EXPECT_EQ(dh.message(), "init timeout");
        }
    }
}

TEST(DeviceHandlersTest, ProviderMetricsMergedFixedKeysWin) {
    StateOverrideMock rt;
    adpp::GetHealthRequest req;
    adpp::Response resp;
    sdk::handlers::handle_get_health(req, resp, rt);
    const auto& m = resp.get_health().provider().metrics();
    ASSERT_TRUE(m.contains("excluded_devices"));
    EXPECT_EQ(m.at("excluded_devices"), "1");
    // The fixed lifecycle key wins over the provider's colliding value.
    ASSERT_TRUE(m.contains("impl"));
    EXPECT_EQ(m.at("impl"), "mock");
}

TEST(DeviceHandlersTest, ProviderHealthDefaultEmitsOnlyFixedKeys) {
    DeviceMock rt;  // no provider_health override
    adpp::GetHealthRequest req;
    adpp::Response resp;
    sdk::handlers::handle_get_health(req, resp, rt);
    const auto& provider = resp.get_health().provider();
    EXPECT_EQ(provider.metrics().size(), 5);  // impl + startup_policy + configured/initialized/failed
    EXPECT_TRUE(provider.metrics().contains("startup_failed_devices"));
    // The startup-derived message is part of the backward-compat surface.
    EXPECT_EQ(provider.message(), "startup degraded: 1 of 2 devices failed to initialize");
}

// A mock that overrides a startup-FAILED id's state (must win over UNREACHABLE)
// and attempts to un-degrade a startup-degraded provider (must be ignored).
namespace {
struct FailedIdStateMock : DeviceMock {
    sdk::DeviceHealthExtra device_health(const std::string& id) const override {
        sdk::DeviceHealthExtra e;
        if (id == "flaky") {
            e.state = adpp::DeviceHealth::STATE_FAULT;
            e.message = "came up then faulted";
        }
        return e;
    }
    sdk::ProviderHealthExtra provider_health() const override {
        sdk::ProviderHealthExtra e;
        e.state = adpp::ProviderHealth::STATE_OK;  // must NOT un-degrade
        e.message = "all good (should be ignored)";
        return e;
    }
};
}  // namespace

TEST(DeviceHandlersTest, DeviceStateOverrideWinsOverFailedIdUnreachable) {
    FailedIdStateMock rt;
    adpp::GetHealthRequest req;
    adpp::Response resp;
    sdk::handlers::handle_get_health(req, resp, rt);
    bool saw_flaky = false;
    for (const auto& dh : resp.get_health().devices()) {
        if (dh.device_id() == "flaky") {
            saw_flaky = true;
            EXPECT_EQ(dh.state(), adpp::DeviceHealth::STATE_FAULT);
            EXPECT_EQ(dh.message(), "came up then faulted");
        }
    }
    EXPECT_TRUE(saw_flaky);
}

TEST(DeviceHandlersTest, ProviderStateOverrideCannotUndegrade) {
    FailedIdStateMock rt;  // startup-degraded; the OK override must be ignored
    adpp::GetHealthRequest req;
    adpp::Response resp;
    sdk::handlers::handle_get_health(req, resp, rt);
    const auto& provider = resp.get_health().provider();
    EXPECT_EQ(provider.state(), adpp::ProviderHealth::STATE_DEGRADED);
    // The ignored escalation must not leave a contradictory "all good" message.
    EXPECT_NE(provider.message(), "all good (should be ignored)");
}

TEST(DeviceHandlersTest, ProviderStateOverrideEscalatesToDegraded) {
    HealthyEscalateMock rt;
    adpp::GetHealthRequest req;
    adpp::Response resp;
    sdk::handlers::handle_get_health(req, resp, rt);
    EXPECT_EQ(resp.get_health().provider().state(), adpp::ProviderHealth::STATE_DEGRADED);
    EXPECT_EQ(resp.get_health().provider().message(), "i2c executor stopped");
}

//=============================================================================
// Request fields echoed in error messages are bounded (#50)
//=============================================================================

namespace {

// Strict UTF-8 check: protobuf string fields must hold valid UTF-8, so a
// truncated echo must not split a character.
bool is_valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80U) {
            len = 1;
        } else if ((c & 0xE0U) == 0xC0U) {
            len = 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            len = 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

// An odd-length ASCII head then 2- and 3-byte characters, so any byte limit
// lands inside a character somewhere.
std::string long_utf8_id() {
    std::string id = "a";
    for (int i = 0; i < 20000; ++i) {
        id += "\xC3\xA9\xE2\x82\xAC";  // e-acute, euro sign
    }
    return id;
}

void expect_bounded(const adpp::Response& resp, adpp::Status::Code code, const std::string& prefix) {
    EXPECT_EQ(resp.status().code(), code);
    const std::string& msg = resp.status().message();
    EXPECT_LE(msg.size(), 256U) << "echo is not bounded";
    EXPECT_TRUE(is_valid_utf8(msg)) << "truncation split a character";
    EXPECT_EQ(msg.rfind(prefix, 0), 0U) << msg.substr(0, 80);
}

}  // namespace

TEST(DeviceHandlersTest, UnknownIdsAreEchoedBounded) {
    DeviceMock rt;
    const std::string id = long_utf8_id();
    {
        adpp::DescribeDeviceRequest req;
        req.set_device_id(id);
        adpp::Response resp;
        sdk::handlers::handle_describe_device(req, resp, rt);
        expect_bounded(resp, adpp::Status::CODE_NOT_FOUND, "unknown device_id: a");
    }
    {
        adpp::ReadSignalsRequest req;
        req.set_device_id(id);
        adpp::Response resp;
        sdk::handlers::handle_read_signals(req, resp, rt);
        expect_bounded(resp, adpp::Status::CODE_NOT_FOUND, "unknown device_id: a");
    }
    {
        adpp::ReadSignalsRequest req;
        req.set_device_id("temp");
        req.add_signal_ids(id);
        adpp::Response resp;
        sdk::handlers::handle_read_signals(req, resp, rt);
        expect_bounded(resp, adpp::Status::CODE_NOT_FOUND, "unknown signal_id 'a");
    }
    {
        adpp::CallRequest req;
        req.set_device_id(id);
        req.set_function_id(1);
        adpp::Response resp;
        sdk::handlers::handle_call(req, resp, rt);
        expect_bounded(resp, adpp::Status::CODE_NOT_FOUND, "unknown device_id: a");
    }
    {
        adpp::CallRequest req;
        req.set_device_id("temp");
        req.set_function_name(id);
        adpp::Response resp;
        sdk::handlers::handle_call(req, resp, rt);
        expect_bounded(resp, adpp::Status::CODE_NOT_FOUND, "unknown function_name 'a");
    }
}

TEST(DeviceHandlersTest, ShortIdsAreEchoedWhole) {
    DeviceMock rt;
    adpp::DescribeDeviceRequest req;
    req.set_device_id("ghost");
    adpp::Response resp;
    sdk::handlers::handle_describe_device(req, resp, rt);
    EXPECT_EQ(resp.status().message(), "unknown device_id: ghost");
}
