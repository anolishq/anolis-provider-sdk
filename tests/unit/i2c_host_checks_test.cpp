/**
 * i2c_host_checks_test.cpp — the reusable i2c-dev host checks (anolis#318).
 *
 * The bus node is stood in for by a plain file and sysfs by a fixture tree; the
 * checks only stat, open and read files, so that is the whole surface.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "anolis/provider_sdk/i2c/host_checks.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace hc = anolis::provider_sdk::host_check;
using anolis::provider_sdk::i2c::check_host;
using anolis::provider_sdk::i2c::HostCheckOptions;

#if defined(__linux__)

namespace {

class I2cHostChecks : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() / (std::string("sdk-i2c-host-") + info->name() + "-" +
                                             std::to_string(static_cast<long long>(::getpid())));
        fs::remove_all(root_);
        fs::create_directories(root_ / "dev");
        bus_ = (root_ / "dev" / "i2c-1").string();
        std::ofstream(bus_).put('\0');
        fs::permissions(bus_, fs::perms::owner_read | fs::perms::owner_write);
        options_.sysfs_root = (root_ / "sys").string();
    }
    void TearDown() override {
        if (!root_.empty()) {
            std::error_code ec;
            fs::permissions(bus_, fs::perms::owner_all, ec);
            fs::remove_all(root_, ec);
        }
    }

    void write_clock(uint32_t hz) {
        const fs::path dir = root_ / "sys" / "class" / "i2c-dev" / "i2c-1" / "device" / "of_node";
        fs::create_directories(dir);
        std::ofstream out(dir / "clock-frequency", std::ios::binary);
        const char be[4] = {static_cast<char>(hz >> 24U), static_cast<char>(hz >> 16U), static_cast<char>(hz >> 8U),
                            static_cast<char>(hz)};
        out.write(be, 4);
    }

    static hc::Requirement find(const std::vector<hc::Requirement>& reqs, const std::string& id) {
        for (const auto& r : reqs) {
            if (r.id == id) {
                return r;
            }
        }
        throw std::runtime_error("no requirement " + id);
    }

    static bool running_as_root() { return ::geteuid() == 0; }

    fs::path root_;
    std::string bus_;
    HostCheckOptions options_;
};

}  // namespace

TEST_F(I2cHostChecks, PresentAndAccessibleWithoutAMaximum) {
    const auto reqs = check_host(bus_, options_);
    ASSERT_EQ(reqs.size(), 2U);  // no maximum: no clock requirement
    EXPECT_EQ(find(reqs, "i2c.bus_present").status, hc::Status::Met);
    EXPECT_EQ(find(reqs, "i2c.bus_access").status, running_as_root() ? hc::Status::Unknown : hc::Status::Met);
}

TEST_F(I2cHostChecks, MissingNodeIsUnmetAndTheRestAreNotChecked) {
    options_.max_bus_hz = 50000;
    const std::string missing = (root_ / "dev" / "i2c-7").string();
    const auto reqs = check_host(missing, options_);
    ASSERT_EQ(reqs.size(), 3U);
    const auto present = find(reqs, "i2c.bus_present");
    EXPECT_EQ(present.status, hc::Status::Unmet);
    EXPECT_NE(present.detail.find(missing), std::string::npos);
    EXPECT_FALSE(present.remedy.empty());
    EXPECT_EQ(find(reqs, "i2c.bus_access").status, hc::Status::Unknown);
    EXPECT_EQ(find(reqs, "i2c.bus_clock").status, hc::Status::Unknown);
    EXPECT_EQ(hc::exit_code(reqs), 1);
}

TEST_F(I2cHostChecks, DeniedAccessNamesTheOwningGroup) {
    if (running_as_root()) {
        GTEST_SKIP() << "root opens the node whatever its mode";
    }
    fs::permissions(bus_, fs::perms::none);
    const auto reqs = check_host(bus_, options_);
    const auto access = find(reqs, "i2c.bus_access");
    EXPECT_EQ(access.status, hc::Status::Unmet);
    EXPECT_NE(access.detail.find("mode 0000"), std::string::npos) << access.detail;
    EXPECT_NE(access.detail.find("group '"), std::string::npos) << access.detail;
    EXPECT_NE(access.remedy.find("group '"), std::string::npos) << access.remedy;
}

TEST_F(I2cHostChecks, ClockWithinTheMaximumIsMet) {
    write_clock(50000);
    options_.max_bus_hz = 50000;
    const auto reqs = check_host(bus_, options_);
    const auto clock = find(reqs, "i2c.bus_clock");
    EXPECT_EQ(clock.status, hc::Status::Met) << clock.detail;
    EXPECT_NE(clock.detail.find("50000 Hz"), std::string::npos) << clock.detail;
}

TEST_F(I2cHostChecks, ClockAboveTheMaximumIsUnmet) {
    write_clock(100000);  // the Pi's default, read big-endian as the device tree stores it
    options_.max_bus_hz = 50000;
    const auto reqs = check_host(bus_, options_);
    const auto clock = find(reqs, "i2c.bus_clock");
    EXPECT_EQ(clock.status, hc::Status::Unmet) << clock.detail;
    EXPECT_NE(clock.detail.find("100000 Hz"), std::string::npos) << clock.detail;
    EXPECT_NE(clock.remedy.find("50000 Hz"), std::string::npos) << clock.remedy;
    EXPECT_EQ(hc::exit_code(reqs), 1);
}

TEST_F(I2cHostChecks, ClockNotExposedIsUnknown) {
    options_.max_bus_hz = 50000;
    EXPECT_EQ(find(check_host(bus_, options_), "i2c.bus_clock").status, hc::Status::Unknown);
}

TEST_F(I2cHostChecks, ClockOnAPathWithNoAdapterNameIsUnknown) {
    const std::string odd = (root_ / "dev" / "bus").string();
    std::ofstream(odd).put('\0');
    options_.max_bus_hz = 50000;
    EXPECT_EQ(find(check_host(odd, options_), "i2c.bus_clock").status, hc::Status::Unknown);
}

#else

TEST(I2cHostChecks, EveryCheckIsUnknownOffLinux) {
    HostCheckOptions options;
    options.max_bus_hz = 50000;
    const auto reqs = check_host("/dev/i2c-1", options);
    ASSERT_EQ(reqs.size(), 3U);
    for (const auto& r : reqs) {
        EXPECT_EQ(r.status, hc::Status::Unknown) << r.id;
    }
}

#endif
