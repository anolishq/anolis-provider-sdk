#include <gtest/gtest.h>

#include <string>

#include "anolis/provider_sdk/version.hpp"
#include "protocol.pb.h"

// Scaffold acceptance smoke test (anolis-protocol#52): proves the SDK builds,
// links, the ADPP proto re-export (anolis::adpp_proto + the protocol.pb.h shim)
// is wired, and the generated version header resolves. The spine/device-model
// API is lifted in later Phase-D steps; this exercises only the scaffold.

TEST(ProtoSmokeTest, CanConstructHelloRequest) {
    anolis::deviceprovider::v1::HelloRequest request;
    request.set_protocol_version("v1");
    request.set_client_name("sdk-proto-smoke-test");

    EXPECT_EQ(request.protocol_version(), "v1");
    EXPECT_EQ(request.client_name(), "sdk-proto-smoke-test");
}

TEST(ProtoSmokeTest, CanPopulateAndSerializeResponse) {
    anolis::deviceprovider::v1::Response response;
    response.set_request_id(1);
    response.mutable_status()->set_code(anolis::deviceprovider::v1::Status::CODE_OK);
    response.mutable_status()->set_message("ok");

    auto* hello = response.mutable_hello();
    hello->set_protocol_version("v1");
    hello->set_provider_name("anolis-provider-sdk-smoke");
    (*hello->mutable_metadata())["transport"] = "stdio+uint32_le";

    std::string payload;
    ASSERT_TRUE(response.SerializeToString(&payload));
    EXPECT_FALSE(payload.empty());
}

TEST(ProtoSmokeTest, RequestRoundTripsThroughSerialization) {
    anolis::deviceprovider::v1::Request request;
    request.set_request_id(42);
    request.mutable_hello()->set_protocol_version("v1");
    request.mutable_hello()->set_client_name("sdk-proto-smoke-test");

    std::string payload;
    ASSERT_TRUE(request.SerializeToString(&payload));

    anolis::deviceprovider::v1::Request parsed;
    ASSERT_TRUE(parsed.ParseFromString(payload));
    EXPECT_EQ(parsed.request_id(), 42u);
    EXPECT_EQ(parsed.hello().client_name(), "sdk-proto-smoke-test");
}

TEST(VersionSmokeTest, RuntimeVersionMatchesCompileTimeConstant) {
    EXPECT_STREQ(anolis::provider_sdk::runtime_version(), anolis::provider_sdk::kVersion);
    EXPECT_GT(std::string(anolis::provider_sdk::kVersion).size(), 0u);
}
