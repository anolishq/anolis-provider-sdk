#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "anolis/provider_sdk/handlers.hpp"
#include "anolis/provider_sdk/runtime.hpp"
#include "anolis/provider_sdk/transport.hpp"
#include "protocol.pb.h"

// D.3b spine tests: the run-loop (§3.2 Hello-gate, dispatch, exit codes, hooks)
// and the handshake/lifecycle handlers, driven through a mock ProviderRuntime.

namespace sdk = anolis::provider_sdk;
namespace adpp = anolis::deviceprovider::v1;

namespace {

// Minimal ProviderRuntime: D.3b only exercises metadata()/readiness(); the
// device-model methods return trivial values (D.3c handlers exercise those).
class MockRuntime : public sdk::ProviderRuntime {
public:
    sdk::ProviderMetadata metadata() const override {
        sdk::ProviderMetadata m;
        m.name = "anolis-provider-mock";
        m.version = "9.9.9";
        m.protocol_version = "v1";
        m.hello_extra["supports_wait_ready"] = "true";
        return m;
    }
    sdk::ReadinessReport readiness() const override {
        sdk::ReadinessReport r;
        r.ready = true;
        r.configured_device_count = 2;
        r.successful_device_ids = {"dev0", "dev1"};
        r.startup_policy = "strict";
        r.provider_impl = "mock";
        return r;
    }
    std::vector<std::string> list_device_ids() const override { return {"dev0", "dev1"}; }
    bool has_device(const std::string& id) const override { return id == "dev0" || id == "dev1"; }
    adpp::Device device_info(const std::string&) const override { return {}; }
    adpp::CapabilitySet capabilities(const std::string&) const override { return {}; }
    sdk::AdapterReadResult read(const std::string&, const std::vector<std::string>&) override { return {}; }
    sdk::AdapterCallResult call(const std::string&, uint32_t, const sdk::ValueMap&) override { return {}; }
    std::optional<uint32_t> resolve_function_id(const std::string&, const std::string&) const override {
        return std::nullopt;
    }
};

// Frame a Request into a stream the way the transport expects.
void push_request(std::ostream& stream, const adpp::Request& req) {
    std::string bytes;
    EXPECT_TRUE(req.SerializeToString(&bytes));
    std::string err;
    ASSERT_TRUE(sdk::transport::write_frame(stream, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), err))
        << err;
}

// Read every framed Response out of a stream.
std::vector<adpp::Response> drain_responses(std::istream& stream) {
    std::vector<adpp::Response> out;
    std::vector<uint8_t> frame;
    std::string err;
    while (sdk::transport::read_frame(stream, frame, err)) {
        adpp::Response r;
        EXPECT_TRUE(r.ParseFromArray(frame.data(), static_cast<int>(frame.size())));
        out.push_back(r);
    }
    EXPECT_TRUE(err.empty()) << err;
    return out;
}

adpp::Request hello_request() {
    adpp::Request req;
    req.set_request_id(1);
    req.mutable_hello()->set_protocol_version("v1");
    req.mutable_hello()->set_client_name("test");
    return req;
}

}  // namespace

TEST(SpineTest, HelloHandshakeSucceedsAndAdvertisesMetadata) {
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());

    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);  // clean EOF after the one frame

    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 1u);
    const auto& r = responses[0];
    EXPECT_EQ(r.request_id(), 1u);
    EXPECT_EQ(r.status().code(), adpp::Status::CODE_OK);
    EXPECT_EQ(r.hello().provider_name(), "anolis-provider-mock");
    EXPECT_EQ(r.hello().provider_version(), "9.9.9");
    EXPECT_EQ(r.hello().metadata().at("transport"), "stdio+uint32_le");
    EXPECT_EQ(r.hello().metadata().at("supports_wait_ready"), "true");
}

TEST(SpineTest, NonHelloBeforeHelloIsRejectedByTheGate) {
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    adpp::Request req;
    req.set_request_id(7);
    req.mutable_wait_ready();  // non-Hello, before any Hello
    push_request(in, req);

    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);
    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].status().code(), adpp::Status::CODE_FAILED_PRECONDITION);
}

TEST(SpineTest, ProtocolVersionMismatchFailsHello) {
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    adpp::Request req;
    req.set_request_id(2);
    req.mutable_hello()->set_protocol_version("v2");  // unsupported
    push_request(in, req);

    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);
    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].status().code(), adpp::Status::CODE_FAILED_PRECONDITION);
}

TEST(SpineTest, WaitReadyAfterHelloProjectsDiagnosticsAndFiresHook) {
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    adpp::Request wr;
    wr.set_request_id(2);
    wr.mutable_wait_ready();
    push_request(in, wr);

    bool wait_ready_fired = false;
    bool shutdown_fired = false;
    sdk::LifecycleHooks hooks;
    hooks.on_wait_ready = [&] { wait_ready_fired = true; };
    hooks.on_shutdown = [&] { shutdown_fired = true; };

    EXPECT_EQ(sdk::run_loop(in, out, rt, hooks), 0);
    EXPECT_TRUE(wait_ready_fired);
    EXPECT_TRUE(shutdown_fired) << "on_shutdown must fire on clean EOF too";

    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 2u);
    const auto& wr_resp = responses[1];
    EXPECT_EQ(wr_resp.status().code(), adpp::Status::CODE_OK);
    EXPECT_EQ(wr_resp.wait_ready().diagnostics().at("device_count"), "2");
    EXPECT_EQ(wr_resp.wait_ready().diagnostics().at("provider_version"), "9.9.9");
    EXPECT_EQ(wr_resp.wait_ready().diagnostics().at("startup_degraded"), "false");
}

// anolis-protocol#76: the initialize hook runs once, after Hello and before
// the first reply that needs the provider started, so Hello never waits on
// it. A runtime whose inventory exists only once the hook has run shows that
// the WaitReady reply comes after it.
class LateRuntime : public MockRuntime {
public:
    sdk::ReadinessReport readiness() const override {
        sdk::ReadinessReport r = MockRuntime::readiness();
        if (!started) {
            r.successful_device_ids.clear();
        }
        return r;
    }
    bool started = false;
};

struct InitializeCalls {
    std::vector<std::chrono::milliseconds> budgets;
    sdk::LifecycleHooks hooks(LateRuntime& rt) {
        sdk::LifecycleHooks h;
        h.initialize = [this, &rt](std::chrono::milliseconds budget) {
            budgets.push_back(budget);
            rt.started = true;
        };
        return h;
    }
};

adpp::Request wait_ready_request(uint64_t id, uint32_t hint_ms) {
    adpp::Request req;
    req.set_request_id(id);
    req.mutable_wait_ready()->set_max_wait_ms_hint(hint_ms);
    return req;
}

TEST(SpineTest, HelloIsAnsweredWithoutRunningInitialize) {
    LateRuntime rt;
    InitializeCalls calls;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());

    EXPECT_EQ(sdk::run_loop(in, out, rt, calls.hooks(rt)), 0);
    EXPECT_TRUE(calls.budgets.empty());
    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].status().code(), adpp::Status::CODE_OK);
    EXPECT_TRUE(responses[0].has_hello());
}

TEST(SpineTest, InitializeRunsOnceBeforeTheWaitReadyReplyWithItsHint) {
    LateRuntime rt;
    InitializeCalls calls;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    push_request(in, wait_ready_request(2, 7000));
    push_request(in, wait_ready_request(3, 7000));

    EXPECT_EQ(sdk::run_loop(in, out, rt, calls.hooks(rt)), 0);
    ASSERT_EQ(calls.budgets.size(), 1u);
    EXPECT_EQ(calls.budgets[0], std::chrono::milliseconds{7000});
    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 3u);
    EXPECT_EQ(responses[1].wait_ready().diagnostics().at("device_count"), "2");
}

TEST(SpineTest, InitializeRunsBeforeAnyFirstRequestWithNoBudget) {
    LateRuntime rt;
    InitializeCalls calls;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    adpp::Request health;
    health.set_request_id(2);
    health.mutable_get_health();
    push_request(in, health);

    EXPECT_EQ(sdk::run_loop(in, out, rt, calls.hooks(rt)), 0);
    ASSERT_EQ(calls.budgets.size(), 1u);
    EXPECT_EQ(calls.budgets[0], std::chrono::milliseconds{0});
}

TEST(SpineTest, ARequestBeforeHelloDoesNotRunInitialize) {
    LateRuntime rt;
    InitializeCalls calls;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, wait_ready_request(1, 7000));

    EXPECT_EQ(sdk::run_loop(in, out, rt, calls.hooks(rt)), 0);
    EXPECT_TRUE(calls.budgets.empty());
}

TEST(SpineTest, UnrecognizedRequestFallsThroughToUnimplemented) {
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    adpp::Request empty;  // no request oneof set -> dispatch fall-through
    empty.set_request_id(3);
    push_request(in, empty);

    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);
    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_EQ(responses[1].status().code(), adpp::Status::CODE_UNIMPLEMENTED);
}

TEST(SpineTest, EmptyInputIsCleanEofExitZero) {
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);
    EXPECT_TRUE(drain_responses(out).empty());
}

TEST(SpineTest, MalformedFrameReturnsParseError) {
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    // A valid frame wrapping non-protobuf garbage.
    const std::string garbage = "\xFF\xFF\xFF not a request";
    std::string err;
    ASSERT_TRUE(sdk::transport::write_frame(in, reinterpret_cast<const uint8_t*>(garbage.data()), garbage.size(), err));

    EXPECT_EQ(sdk::run_loop(in, out, rt), 3);  // parse error
}

// #50: a response over the 1 MiB frame cap.

namespace {

std::string serialized(const adpp::Request& req) {
    std::string bytes;
    EXPECT_TRUE(req.SerializeToString(&bytes));
    return bytes;
}

// Every response frame fits the cap and is not OK/UNSPECIFIED where expected.
void expect_error_frame(const adpp::Response& r, uint64_t request_id) {
    EXPECT_EQ(r.request_id(), request_id);
    EXPECT_NE(r.status().code(), adpp::Status::CODE_OK);
    EXPECT_NE(r.status().code(), adpp::Status::CODE_UNSPECIFIED);
    EXPECT_LE(r.ByteSizeLong(), sdk::transport::kMaxFrameBytes);
}

// device_info() carries a label too large for any one frame, so a ListDevices
// answer cannot fit however the handler phrases it.
class HugeDeviceRuntime : public MockRuntime {
public:
    adpp::Device device_info(const std::string& id) const override {
        adpp::Device d;
        d.set_device_id(id);
        d.set_label(std::string(sdk::transport::kMaxFrameBytes, 'L'));
        return d;
    }
};

}  // namespace

TEST(SpineTest, SubCapRequestWithAnOverCapAnswerGetsAnErrorAndTheLoopContinues) {
    // The issue's repro: DescribeDevice with an unknown device_id of 1048560
    // bytes. The request is under the cap; the NOT_FOUND message echoing it
    // was not, and the provider exited 5 without answering.
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    adpp::Request describe;
    describe.set_request_id(2);
    describe.mutable_describe_device()->set_device_id(std::string(1048560, 'x'));
    ASSERT_LT(serialized(describe).size(), sdk::transport::kMaxFrameBytes);
    push_request(in, describe);
    adpp::Request list;
    list.set_request_id(3);
    list.mutable_list_devices();
    push_request(in, list);

    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);
    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 3u);
    expect_error_frame(responses[1], 2);
    EXPECT_EQ(responses[1].status().code(), adpp::Status::CODE_NOT_FOUND) << responses[1].status().message().size();
    EXPECT_EQ(responses[2].request_id(), 3u);
    EXPECT_EQ(responses[2].status().code(), adpp::Status::CODE_OK);
}

TEST(SpineTest, AnOverCapResponseIsReplacedByResourceExhausted) {
    // Whatever produces it (here a provider's own device_info), a response over
    // the cap is answered with a small error for the same request_id.
    HugeDeviceRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    adpp::Request list;
    list.set_request_id(41);
    list.mutable_list_devices();
    push_request(in, list);
    adpp::Request health;
    health.set_request_id(42);
    health.mutable_get_health();
    push_request(in, health);

    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);
    const auto responses = drain_responses(out);
    ASSERT_EQ(responses.size(), 3u);
    expect_error_frame(responses[1], 41);
    EXPECT_EQ(responses[1].status().code(), adpp::Status::CODE_RESOURCE_EXHAUSTED);
    EXPECT_FALSE(responses[1].has_list_devices());
    EXPECT_EQ(responses[2].request_id(), 42u);
    EXPECT_EQ(responses[2].status().code(), adpp::Status::CODE_OK);
}

namespace {

// One device whose label length is chosen by the test, to size a ListDevices
// response to the byte.
class SizedDeviceRuntime : public MockRuntime {
public:
    std::size_t label_bytes = 0;
    std::vector<std::string> list_device_ids() const override { return {"dev0"}; }
    adpp::Device device_info(const std::string& id) const override {
        adpp::Device d;
        d.set_device_id(id);
        d.set_label(std::string(label_bytes, 'L'));
        return d;
    }
};

// The serialized size of run_loop's answer to ListDevices (request_id 7).
std::size_t list_devices_response_bytes(SizedDeviceRuntime& rt) {
    adpp::Response r;
    r.set_request_id(7);
    sdk::handlers::handle_list_devices(adpp::ListDevicesRequest{}, r, rt);
    return r.ByteSizeLong();
}

// Runs Hello + ListDevices(request_id 7) and returns the ListDevices answer.
adpp::Response list_devices_through_run_loop(SizedDeviceRuntime& rt) {
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    adpp::Request list;
    list.set_request_id(7);
    list.mutable_list_devices();
    push_request(in, list);
    EXPECT_EQ(sdk::run_loop(in, out, rt), 0);
    const auto responses = drain_responses(out);
    EXPECT_EQ(responses.size(), 2u);
    return responses.empty() ? adpp::Response{} : responses.back();
}

}  // namespace

TEST(SpineTest, AResponseOfExactlyTheCapIsSentUnchanged) {
    // The cap is inclusive (write_frame accepts size == kMaxFrameBytes): a
    // response of exactly 1048576 bytes is legal and goes out as it is; one
    // byte more is replaced.
    SizedDeviceRuntime rt;
    rt.label_bytes = 1048000;
    const std::size_t probe = list_devices_response_bytes(rt);
    rt.label_bytes += sdk::transport::kMaxFrameBytes - probe;  // the varint prefixes stay 3 bytes at this size
    ASSERT_EQ(list_devices_response_bytes(rt), sdk::transport::kMaxFrameBytes);

    const adpp::Response at_cap = list_devices_through_run_loop(rt);
    EXPECT_EQ(at_cap.request_id(), 7u);
    EXPECT_EQ(at_cap.status().code(), adpp::Status::CODE_OK);
    ASSERT_EQ(at_cap.list_devices().devices_size(), 1);
    EXPECT_EQ(at_cap.list_devices().devices(0).label().size(), rt.label_bytes);

    rt.label_bytes += 1;
    ASSERT_EQ(list_devices_response_bytes(rt), sdk::transport::kMaxFrameBytes + 1);
    const adpp::Response over = list_devices_through_run_loop(rt);
    EXPECT_EQ(over.request_id(), 7u);
    EXPECT_EQ(over.status().code(), adpp::Status::CODE_RESOURCE_EXHAUSTED);
}

TEST(SpineTest, AFailedWriteIsExitFive) {
    // Exit 5 stays for a real output-stream failure.
    MockRuntime rt;
    std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
    push_request(in, hello_request());
    std::ostream broken(nullptr);  // no buffer: every write sets badbit
    EXPECT_EQ(sdk::run_loop(in, broken, rt), 5);
}
