/**
 * host_check_test.cpp — opaque claims and the `--check-host` envelope
 * (anolis#318, executable profile v1 §6).
 */

#include "anolis/provider_sdk/host_check.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "anolis/provider_sdk/claims.hpp"
#include "anolis/provider_sdk/i2c/claims.hpp"

namespace sdk = anolis::provider_sdk;
namespace hc = anolis::provider_sdk::host_check;

namespace {

std::string claim_tag(const sdk::adpp::Device& device) {
    const auto it = device.tags().find(std::string(sdk::kClaimTag));
    return it == device.tags().end() ? std::string() : it->second;
}

}  // namespace

TEST(Claims, AddsKeysSpaceSeparatedWithoutRepeats) {
    sdk::adpp::Device device;
    (*device.mutable_tags())["other"] = "kept";
    sdk::add_claim(device, "i2c:/dev/i2c-1:0x0a");
    sdk::add_claim(device, "usb:1-1.2");
    sdk::add_claim(device, "i2c:/dev/i2c-1:0x0a");
    EXPECT_EQ(claim_tag(device), "i2c:/dev/i2c-1:0x0a usb:1-1.2");
    EXPECT_EQ(device.tags().at("other"), "kept");
}

TEST(Claims, RejectsEmptyOrWhitespaceKeys) {
    sdk::adpp::Device device;
    EXPECT_THROW(sdk::add_claim(device, ""), std::invalid_argument);
    EXPECT_THROW(sdk::add_claim(device, "a b"), std::invalid_argument);
    EXPECT_THROW(sdk::add_claim(device, "a\tb"), std::invalid_argument);
    EXPECT_EQ(claim_tag(device), "");
}

TEST(I2cClaimKey, MatchesTheRuntimesCanonicalForm) {
    // Lowercase, two-digit hex: what the runtime normalized `hw.i2c_address` to,
    // so providers formatting "0x0A" and "0xa" now agree by construction.
    EXPECT_EQ(sdk::i2c::claim_key("/dev/i2c-1", 0x0A), "i2c:/dev/i2c-1:0x0a");
    EXPECT_EQ(sdk::i2c::claim_key("/dev/i2c-1", 0x7F), "i2c:/dev/i2c-1:0x7f");
    EXPECT_EQ(sdk::i2c::claim_key("/dev/i2c-1", 0x00), "i2c:/dev/i2c-1:0x00");
}

TEST(I2cClaimKey, RejectsWhatCannotBeAClaim) {
    EXPECT_THROW(sdk::i2c::claim_key("", 0x14), std::invalid_argument);
    EXPECT_THROW(sdk::i2c::claim_key("/dev/i2c 1", 0x14), std::invalid_argument);
    EXPECT_THROW(sdk::i2c::claim_key("/dev/i2c-1", 0x80), std::invalid_argument);
}

TEST(HostCheck, ExitCodeIsOneExactlyWhenSomethingIsUnmet) {
    EXPECT_EQ(hc::exit_code({}), 0);
    EXPECT_EQ(hc::exit_code({{"a", hc::Status::Met, "", ""}, {"b", hc::Status::Unknown, "", ""}}), 0);
    EXPECT_EQ(hc::exit_code({{"a", hc::Status::Met, "", ""}, {"b", hc::Status::Unmet, "", ""}}), 1);
}

TEST(HostCheck, EnvelopeShape) {
    const std::vector<hc::Requirement> reqs = {
        {"bus.present", hc::Status::Met, "/dev/i2c-1 exists", ""},
        {"bus.access", hc::Status::Unmet, "denied", "add the user to group 'i2c'"},
        {"bus.clock", hc::Status::Unknown, "", ""},
    };
    EXPECT_EQ(hc::envelope("anolis-provider-x", reqs),
              "{\"check_host_version\":1,\"provider\":\"anolis-provider-x\",\"requirements\":["
              "{\"id\":\"bus.present\",\"status\":\"met\",\"detail\":\"/dev/i2c-1 exists\"},"
              "{\"id\":\"bus.access\",\"status\":\"unmet\",\"detail\":\"denied\","
              "\"remedy\":\"add the user to group 'i2c'\"},"
              "{\"id\":\"bus.clock\",\"status\":\"unknown\"}]}");
    EXPECT_EQ(hc::envelope("", {}), "{\"check_host_version\":1,\"requirements\":[]}");
}

TEST(HostCheck, EnvelopeEscapesAndRepairsText) {
    // Host text (paths, strerror) is escaped, and bytes that are not UTF-8 are
    // replaced, so the envelope always parses.
    const std::string valid_utf8 = "caf\xC3\xA9";
    const std::vector<hc::Requirement> reqs = {
        {"x", hc::Status::Met, std::string("q\"b\\n\nt\x01") + valid_utf8 + "\xFF" + "\xC3", ""},
    };
    EXPECT_EQ(hc::envelope("", reqs),
              "{\"check_host_version\":1,\"requirements\":[{\"id\":\"x\",\"status\":\"met\",\"detail\":"
              "\"q\\\"b\\\\n\\nt\\u0001caf\xC3\xA9\\ufffd\\ufffd\"}]}");
}

TEST(HostCheck, EnvelopeRejectsAnEmptyId) {
    EXPECT_THROW(hc::envelope("", {{"", hc::Status::Met, "", ""}}), std::invalid_argument);
}

TEST(HostCheck, WriteEnvelopePrintsOneLineAndReturnsTheExitCode) {
    std::ostringstream out;
    EXPECT_EQ(hc::write_envelope(out, "p", {{"a", hc::Status::Unmet, "", ""}}), 1);
    EXPECT_EQ(out.str(), hc::envelope("p", {{"a", hc::Status::Unmet, "", ""}}) + "\n");
}

TEST(HostCheck, ReadinessDiagnostics) {
    const auto ok = hc::readiness_diagnostics({{"a", hc::Status::Met, "", ""}, {"b", hc::Status::Unknown, "", ""}});
    EXPECT_EQ(ok.at("host_check"), "ok");
    EXPECT_EQ(ok.count("host_unmet"), 0U);

    const auto unmet = hc::readiness_diagnostics(
        {{"a", hc::Status::Unmet, "missing", "fix"}, {"b", hc::Status::Met, "", ""}, {"c", hc::Status::Unmet, "", ""}});
    EXPECT_EQ(unmet.at("host_check"), "unmet");
    EXPECT_EQ(unmet.at("host_unmet"), "a: missing; c");
}
