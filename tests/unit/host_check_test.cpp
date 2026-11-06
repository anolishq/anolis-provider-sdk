/**
 * host_check_test.cpp — opaque claims and the `--check-host` envelope
 * (anolis#318, executable profile v1 §6).
 */

#include "anolis/provider_sdk/host_check.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
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

namespace {

// The last key claim_keys gives: the canonical one (the only one when the
// configured path is already canonical).
std::string canonical_key(const std::string& path, unsigned int address) {
    return sdk::i2c::claim_keys(path, address).back();
}

// A device publishing every key claim_keys gives (a provider on this release).
sdk::adpp::Device new_sdk_device(const std::string& path, unsigned int address) {
    sdk::adpp::Device device;
    for (const auto& key : sdk::i2c::claim_keys(path, address)) {
        sdk::add_claim(device, key);
    }
    return device;
}

// A device publishing the configured key alone (a provider on 0.6.0 or earlier).
sdk::adpp::Device old_sdk_device(const std::string& path, unsigned int address) {
    sdk::adpp::Device device;
    sdk::add_claim(device, sdk::i2c::claim_key(path, address));
    return device;
}

// The runtime's rule (anolis core/runtime/ownership_validation.cpp): split each
// device's claim tag on whitespace; two devices conflict when they share a key.
std::set<std::string> published(const sdk::adpp::Device& device) {
    std::set<std::string> keys;
    std::istringstream in(claim_tag(device));
    std::string key;
    while (in >> key) {
        keys.insert(key);
    }
    return keys;
}

bool collide(const sdk::adpp::Device& a, const sdk::adpp::Device& b) {
    const auto ka = published(a);
    const auto kb = published(b);
    return std::any_of(ka.begin(), ka.end(), [&kb](const std::string& k) { return kb.count(k) != 0U; });
}

}  // namespace

TEST(I2cClaimKey, ClaimKeyIsTheConfiguredPath) {
    // claim_key keeps its 0.6.0 meaning, so a provider that bumps the SDK
    // without moving to claim_keys publishes exactly what it did before.
    EXPECT_EQ(sdk::i2c::claim_key("/dev//i2c-1", 0x63), "i2c:/dev//i2c-1:0x63");
    EXPECT_EQ(sdk::i2c::claim_keys("/dev//i2c-1", 0x63).front(), "i2c:/dev//i2c-1:0x63");
}

TEST(I2cClaimKey, AnOldSdkKeyCollidesWithTheNewSet) {
    // A collision 0.6.0 catches (same configured string) is still caught when
    // one provider moves to claim_keys and the other has not.
    for (const char* path : {"/dev//i2c-1", "/dev/./i2c-1", "/dev/i2c-1"}) {
        EXPECT_TRUE(collide(old_sdk_device(path, 0x63), new_sdk_device(path, 0x63))) << path;
        EXPECT_FALSE(collide(old_sdk_device(path, 0x63), new_sdk_device(path, 0x64))) << path;
    }
    // Two providers on this release with different spellings share the canonical key.
    EXPECT_TRUE(collide(new_sdk_device("/dev//i2c-1", 0x63), new_sdk_device("/dev/./i2c-1", 0x63)));
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
    EXPECT_THROW(sdk::i2c::claim_keys("", 0x14), std::invalid_argument);
    EXPECT_THROW(sdk::i2c::claim_keys("/dev/i2c 1", 0x14), std::invalid_argument);
    EXPECT_THROW(sdk::i2c::claim_keys("/dev/i2c-1", 0x80), std::invalid_argument);
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
        return sdk::i2c::claim_key((dir_ / "i2c-1").generic_string(), address);  // already canonical
    }

    fs::path dir_;
    bool created_ = false;
};

}  // namespace

TEST_F(I2cClaimKeyPaths, RedundantSeparatorsAndDotSegmentsGiveTheNodesKey) {
    const std::string want = node_key(0x63);
    EXPECT_EQ(canonical_key(at("i2c-1"), 0x63), want);
    EXPECT_EQ(canonical_key(at("/i2c-1"), 0x63), want);   // doubled separator
    EXPECT_EQ(canonical_key(at("./i2c-1"), 0x63), want);  // `.` segment
    EXPECT_EQ(canonical_key(at("i2c-1/"), 0x63), want);   // trailing separator
    ASSERT_TRUE(fs::create_directory(dir_ / "sub"));
    EXPECT_EQ(canonical_key(at("sub/../i2c-1"), 0x63), want);  // `..` segment
}

TEST_F(I2cClaimKeyPaths, ClaimKeysGiveTheConfiguredKeyThenTheCanonicalOneOnce) {
    // Already canonical: one key.
    EXPECT_EQ(sdk::i2c::claim_keys(at("i2c-1"), 0x63), std::vector<std::string>{node_key(0x63)});
    // A different spelling: the configured key, then the canonical key.
    const std::vector<std::string> keys = sdk::i2c::claim_keys(at("./i2c-1"), 0x63);
    EXPECT_EQ(keys, (std::vector<std::string>{"i2c:" + at("./i2c-1") + ":0x63", node_key(0x63)}));
    // Published through add_claim, each key once.
    sdk::adpp::Device device;
    for (int round = 0; round < 2; ++round) {
        for (const auto& key : keys) {
            sdk::add_claim(device, key);
        }
    }
    EXPECT_EQ(claim_tag(device), keys[0] + " " + keys[1]);
}

TEST(I2cClaimKey, SpellingsOfDevI2c1GiveOneKey) {
    // The issue's three spellings, whether or not this host has /dev/i2c-1.
    const std::string want = canonical_key("/dev/i2c-1", 0x63);
    EXPECT_EQ(canonical_key("/dev//i2c-1", 0x63), want);
    EXPECT_EQ(canonical_key("/dev/./i2c-1", 0x63), want);
}

TEST(I2cClaimKey, AMissingNodeGetsItsLexicalForm) {
    // Nothing to resolve: the key is the lexically normal path, so the behaviour
    // is defined (aliases of a missing node are only caught if they normalise
    // to the same string).
    EXPECT_EQ(canonical_key("/nonexistent-anolis-sdk-test//x/./y/../i2c-9/", 0x0A),
              "i2c:/nonexistent-anolis-sdk-test/x/i2c-9:0x0a");
}

TEST(I2cClaimKey, UnresolvedRelativePathsCanShareAKey) {
    // Documented in claims.hpp: relative keys do not record the working
    // directory, so these two meet on `i2c:<name>:0x63` wherever they run.
    const std::string name = "anolis-sdk-no-such-node-i2c-1";
    const auto spelled = sdk::i2c::claim_keys("anolis-sdk-no-such-dir/../" + name, 0x63);
    EXPECT_EQ(spelled.back(), "i2c:" + name + ":0x63");
    EXPECT_TRUE(collide(new_sdk_device("anolis-sdk-no-such-dir/../" + name, 0x63), old_sdk_device(name, 0x63)));
}

TEST(I2cClaimKey, SchemePathsAreLeftAlone) {
    // One key, exactly as configured, for any scheme (not only mock://).
    for (const char* path : {"mock://bench", "mock://bench//./x/", "tcp://bridge//./x/"}) {
        EXPECT_EQ(sdk::i2c::claim_keys(path, 0x0A), std::vector<std::string>{"i2c:" + std::string(path) + ":0x0a"})
            << path;
    }
}

TEST(I2cClaimKey, TheRootStaysTheRoot) {
    // Stripping trailing separators must leave `/` itself.
    EXPECT_EQ(canonical_key("/", 0x63), "i2c:/:0x63");
    EXPECT_EQ(canonical_key("//", 0x63), "i2c:/:0x63");
}

#ifndef _WIN32
TEST_F(I2cClaimKeyPaths, ASymlinkGivesItsTargetsKey) {
    // A udev-style alias: `i2c-sensors -> i2c-1`.
    fs::create_symlink("i2c-1", dir_ / "i2c-sensors");
    EXPECT_EQ(canonical_key(at("i2c-sensors"), 0x63), node_key(0x63));
    EXPECT_NE(canonical_key(at("i2c-sensors"), 0x64), node_key(0x63));  // the address still counts
}

TEST_F(I2cClaimKeyPaths, AnAliasCollidesAcrossSdkVersionsWhereOneSideMatches) {
    fs::create_symlink("i2c-1", dir_ / "i2c-sensors");
    const std::string alias = at("i2c-sensors");
    const std::string target = at("i2c-1");
    // Both on this release: alias and target share the canonical key.
    EXPECT_TRUE(collide(new_sdk_device(alias, 0x63), new_sdk_device(target, 0x63)));
    // Old SDK on the same alias string: the configured keys match, as before.
    EXPECT_TRUE(collide(old_sdk_device(alias, 0x63), new_sdk_device(alias, 0x63)));
    // Old SDK on the target: its key is the new provider's canonical key.
    EXPECT_TRUE(collide(old_sdk_device(target, 0x63), new_sdk_device(alias, 0x63)));
    // The gap that remains: an old provider on the alias publishes only the
    // alias string, which the new provider on the target does not publish.
    EXPECT_FALSE(collide(old_sdk_device(alias, 0x63), new_sdk_device(target, 0x63)));
}

TEST_F(I2cClaimKeyPaths, AMissingNodeUnderASymlinkedDirectoryResolvesTheDirectory) {
    fs::create_directory_symlink(dir_, dir_.parent_path() / (dir_.filename().string() + "-link"));
    const fs::path link = dir_.parent_path() / (dir_.filename().string() + "-link");
    const std::string via_link = canonical_key(link.generic_string() + "/i2c-7", 0x0A);
    fs::remove(link);
    EXPECT_EQ(via_link, "i2c:" + fs::canonical(dir_).generic_string() + "/i2c-7:0x0a");
}

TEST_F(I2cClaimKeyPaths, APathThatResolvesToWhitespaceIsRejected) {
    std::ofstream(dir_ / "has space").put('\n');
    fs::create_symlink("has space", dir_ / "bus");
    EXPECT_THROW(sdk::i2c::claim_keys(at("bus"), 0x0A), std::invalid_argument);
    EXPECT_EQ(sdk::i2c::claim_key(at("bus"), 0x0A), "i2c:" + at("bus") + ":0x0a");  // the configured key is fine
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
