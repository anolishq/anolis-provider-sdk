/**
 * host_check_test.cpp — opaque claims and the `--check-host` envelope
 * (anolis#318, executable profile v1 §6).
 */

#include "anolis/provider_sdk/host_check.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
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

namespace {

namespace fs = std::filesystem;

// A fresh directory under the temp dir ($TMPDIR on POSIX), removed afterwards.
class I2cClaimKeyPaths : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        dir_ = fs::temp_directory_path() / ("anolis-sdk-claim-key-" + std::to_string(rd()) + std::to_string(rd()));
        ASSERT_TRUE(fs::create_directory(dir_)) << dir_;
        created_ = true;
        // Canonical up front (Windows' temp dir can be an 8.3 short name), so the
        // only aliasing in a test is the aliasing it builds.
        dir_ = fs::canonical(dir_);
        std::ofstream(dir_ / "i2c-1").put('\n');  // stands in for the device node
        ASSERT_TRUE(fs::exists(dir_ / "i2c-1"));
    }
    void TearDown() override {
        if (created_) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }
    // The path as a string, with forward slashes (the form the key uses).
    [[nodiscard]] std::string at(const std::string& rel) const { return dir_.generic_string() + "/" + rel; }
    [[nodiscard]] std::string node_key(unsigned int address) const {
        return sdk::i2c::claim_key((dir_ / "i2c-1").generic_string(), address);
    }

    fs::path dir_;
    bool created_ = false;
};

}  // namespace

TEST_F(I2cClaimKeyPaths, RedundantSeparatorsAndDotSegmentsGiveTheNodesKey) {
    const std::string want = node_key(0x63);
    EXPECT_EQ(sdk::i2c::claim_key(at("i2c-1"), 0x63), want);
    EXPECT_EQ(sdk::i2c::claim_key(at("/i2c-1"), 0x63), want);   // doubled separator
    EXPECT_EQ(sdk::i2c::claim_key(at("./i2c-1"), 0x63), want);  // `.` segment
    EXPECT_EQ(sdk::i2c::claim_key(at("i2c-1/"), 0x63), want);   // trailing separator
    ASSERT_TRUE(fs::create_directory(dir_ / "sub"));
    EXPECT_EQ(sdk::i2c::claim_key(at("sub/../i2c-1"), 0x63), want);  // `..` segment
}

TEST(I2cClaimKey, SpellingsOfDevI2c1GiveOneKey) {
    // The issue's three spellings, whether or not this host has /dev/i2c-1.
    const std::string want = sdk::i2c::claim_key("/dev/i2c-1", 0x63);
    EXPECT_EQ(sdk::i2c::claim_key("/dev//i2c-1", 0x63), want);
    EXPECT_EQ(sdk::i2c::claim_key("/dev/./i2c-1", 0x63), want);
}

TEST(I2cClaimKey, AMissingNodeGetsItsLexicalForm) {
    // Nothing to resolve: the key is the lexically normal path, so the behaviour
    // is defined (aliases of a missing node are only caught if they normalise
    // to the same string).
    EXPECT_EQ(sdk::i2c::claim_key("/nonexistent-anolis-sdk-test//x/./y/../i2c-9/", 0x0A),
              "i2c:/nonexistent-anolis-sdk-test/x/i2c-9:0x0a");
}

TEST(I2cClaimKey, SchemePathsAreLeftAlone) {
    EXPECT_EQ(sdk::i2c::claim_key("mock://bench", 0x0A), "i2c:mock://bench:0x0a");
    EXPECT_EQ(sdk::i2c::claim_key("mock://bench//./x/", 0x0A), "i2c:mock://bench//./x/:0x0a");
}

#ifndef _WIN32
TEST_F(I2cClaimKeyPaths, ASymlinkGivesItsTargetsKey) {
    // A udev-style alias: `i2c-sensors -> i2c-1`.
    fs::create_symlink("i2c-1", dir_ / "i2c-sensors");
    EXPECT_EQ(sdk::i2c::claim_key(at("i2c-sensors"), 0x63), node_key(0x63));
    EXPECT_NE(sdk::i2c::claim_key(at("i2c-sensors"), 0x64), node_key(0x63));  // the address still counts
}

TEST_F(I2cClaimKeyPaths, AMissingNodeUnderASymlinkedDirectoryResolvesTheDirectory) {
    fs::create_directory_symlink(dir_, dir_.parent_path() / (dir_.filename().string() + "-link"));
    const fs::path link = dir_.parent_path() / (dir_.filename().string() + "-link");
    const std::string via_link = sdk::i2c::claim_key(link.generic_string() + "/i2c-7", 0x0A);
    fs::remove(link);
    EXPECT_EQ(via_link, "i2c:" + fs::canonical(dir_).generic_string() + "/i2c-7:0x0a");
}

TEST_F(I2cClaimKeyPaths, APathThatResolvesToWhitespaceIsRejected) {
    std::ofstream(dir_ / "has space").put('\n');
    fs::create_symlink("has space", dir_ / "bus");
    EXPECT_THROW(sdk::i2c::claim_key(at("bus"), 0x0A), std::invalid_argument);
}
#endif

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
