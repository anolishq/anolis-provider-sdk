#include <gtest/gtest.h>

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
