#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
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
