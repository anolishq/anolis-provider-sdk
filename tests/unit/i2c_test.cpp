/**
 * i2c_test.cpp — shared I2C seam: fault-injection decorator, spec parser,
 * and IoStats accounting (anolis-provider-sdk#19).
 *
 * LinuxI2cBus's real ioctl mechanics are validated on hardware in the provider
 * migrations; here we exercise the protocol-agnostic, fully-deterministic parts.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

#include "anolis/provider_sdk/i2c/fault_injecting_i2c_bus.hpp"
#include "anolis/provider_sdk/i2c/io_stats.hpp"

using namespace anolis::provider_sdk::i2c;

namespace {

// Deterministic inner bus: reads/write_then_read return a canned payload.
class FakeInnerBus final : public I2cBus {
public:
    std::vector<uint8_t> reply{0x01, 0x02, 0x03, 0x04};
    I2cStatus read_status = I2cStatus::ok();
    I2cStatus write_status = I2cStatus::ok();
    int write_calls = 0;
    int read_calls = 0;

    I2cStatus open() override { return I2cStatus::ok(); }
    void close() override {}
    bool is_open() const override { return true; }
    const std::string &bus_path() const override { return path_; }
    void delay_us(uint32_t) override {}
    IoStats io_stats_for(uint8_t address) const override { return io_.stats_for(address); }

    I2cStatus write(uint8_t address, const uint8_t *, size_t) override {
        ++write_calls;
        io_.record(address, static_cast<bool>(write_status), 1);
        return write_status;
    }
    I2cStatus read(uint8_t address, uint8_t *rx, size_t rx_len, size_t *got, uint32_t) override {
        ++read_calls;
        if (!read_status) {
            io_.record(address, false, 1);
            return read_status;
        }
        io_.record(address, true, 1);
        return fill(rx, rx_len, got);
    }
    I2cStatus write_then_read(uint8_t address, const uint8_t *, size_t, uint8_t *rx, size_t rx_len,
                              size_t *got) override {
        const bool ok = static_cast<bool>(write_status) && static_cast<bool>(read_status);
        io_.record(address, ok, 1);
        if (!write_status) return write_status;
        if (!read_status) return read_status;
        return fill(rx, rx_len, got);
    }

private:
    I2cStatus fill(uint8_t *rx, size_t rx_len, size_t *got) {
        const size_t n = std::min(reply.size(), rx_len);
        std::memcpy(rx, reply.data(), n);
        if (got != nullptr) *got = n;
        return I2cStatus::ok();
    }
    std::string path_ = "mock://fake";
    IoStatsMap io_;
};

std::unique_ptr<FaultInjectingI2cBus> wrap(FaultSpec spec, FakeInnerBus **out = nullptr) {
    auto inner = std::make_unique<FakeInnerBus>();
    if (out != nullptr) *out = inner.get();
    return std::make_unique<FaultInjectingI2cBus>(std::move(inner), spec);
}

}  // namespace

// ---- spec parsing --------------------------------------------------------

TEST(FaultSpecParse, ExtractsAllKnownFields) {
    FaultSpec s = FaultSpec::parse(
        "pad=8&pad_byte=0xAB&short_every=4&corrupt_every=6&nak_every=3&read_fail_every=5&timeout_every=7&drop_after="
        "100&drop_for=50&latency_us=500");
    EXPECT_EQ(s.pad, 8u);
    EXPECT_EQ(s.pad_byte, 0xABu);
    EXPECT_EQ(s.short_every, 4u);
    EXPECT_EQ(s.corrupt_every, 6u);
    EXPECT_EQ(s.nak_every, 3u);
    EXPECT_EQ(s.read_fail_every, 5u);
    EXPECT_EQ(s.timeout_every, 7u);
    EXPECT_EQ(s.drop_after, 100u);
    EXPECT_EQ(s.drop_for, 50u);
    EXPECT_EQ(s.latency_us, 500u);
    EXPECT_TRUE(s.any());
}

TEST(FaultSpecParse, EmptyAndUnknownAndGarbage) {
    FaultSpec s = FaultSpec::parse("");
    EXPECT_FALSE(s.any());
    s = FaultSpec::parse("unknown=5&pad=notanumber&nak_every=2");
    EXPECT_EQ(s.pad, 0u);        // garbage value leaves default
    EXPECT_EQ(s.nak_every, 2u);  // valid one still parsed
}

TEST(FaultSpecParse, SplitBusQuery) {
    auto [p1, q1] = split_bus_query("mock://bus?pad=8&nak_every=3");
    EXPECT_EQ(p1, "mock://bus");
    EXPECT_EQ(q1, "pad=8&nak_every=3");
    auto [p2, q2] = split_bus_query("mock://bus");
    EXPECT_EQ(p2, "mock://bus");
    EXPECT_EQ(q2, "");
}

// ---- pass-through --------------------------------------------------------

TEST(FaultInjection, PassThroughWhenNoFaults) {
    auto bus = wrap(FaultSpec{});
    uint8_t rx[16] = {0};
    size_t got = 0;
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_EQ(got, 4u);
    EXPECT_EQ(rx[0], 0x01);
    EXPECT_EQ(rx[3], 0x04);
}

// ---- read mutations ------------------------------------------------------

TEST(FaultInjection, PaddingAppendsBytes) {
    FaultSpec s;
    s.pad = 4;  // pad_byte default 0xFF
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    ASSERT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_EQ(got, 8u);  // 4 frame + 4 padding
    EXPECT_EQ(rx[3], 0x04);
    EXPECT_EQ(rx[4], 0xFF);
    EXPECT_EQ(rx[7], 0xFF);
}

TEST(FaultInjection, PaddingCappedAtBufferCapacity) {
    FaultSpec s;
    s.pad = 100;
    auto bus = wrap(s);
    uint8_t rx[6] = {0};
    size_t got = 0;
    ASSERT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_EQ(got, 6u);  // 4 frame + 2 padding (capped)
}

TEST(FaultInjection, ShortEveryTruncates) {
    FaultSpec s;
    s.short_every = 1;  // every read short by one
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    ASSERT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_EQ(got, 3u);
}

TEST(FaultInjection, CorruptEveryFlipsFirstByte) {
    FaultSpec s;
    s.corrupt_every = 1;
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    ASSERT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_EQ(rx[0], static_cast<uint8_t>(0x01 ^ 0xFF));
    EXPECT_EQ(rx[1], 0x02);  // only the first byte flipped
}

// ---- injected failures ---------------------------------------------------

TEST(FaultInjection, NakEveryFailsNthWrite) {
    FaultSpec s;
    s.nak_every = 2;
    FakeInnerBus *inner = nullptr;
    auto bus = wrap(s, &inner);
    EXPECT_TRUE(bus->write(0x10, nullptr, 1));                            // 1 ok
    EXPECT_EQ(bus->write(0x10, nullptr, 1).code, I2cError::WriteFailed);  // 2 NAK
    EXPECT_TRUE(bus->write(0x10, nullptr, 1));                            // 3 ok
    EXPECT_EQ(bus->write(0x10, nullptr, 1).code, I2cError::WriteFailed);  // 4 NAK
    EXPECT_EQ(inner->write_calls, 2);                                     // failed writes never reach the inner bus
}

TEST(FaultInjection, ReadFailEveryFailsNthRead) {
    FaultSpec s;
    s.read_fail_every = 3;
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_EQ(bus->read(0x10, rx, sizeof(rx), &got, 1000).code, I2cError::ReadFailed);  // 3rd
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
}

TEST(FaultInjection, TimeoutEvery) {
    FaultSpec s;
    s.timeout_every = 2;
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    EXPECT_EQ(bus->read(0x10, rx, sizeof(rx), &got, 1000).code, I2cError::Timeout);  // 2nd op
}

TEST(FaultInjection, DropoutWindowThenRecovers) {
    FaultSpec s;
    s.drop_after = 2;
    s.drop_for = 3;
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));  // op 1 ok
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));  // op 2 ok
    for (int i = 0; i < 3; ++i) {                              // ops 3,4,5 dropped
        EXPECT_EQ(bus->read(0x10, rx, sizeof(rx), &got, 1000).code, I2cError::BusError);
    }
    EXPECT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));  // op 6 recovered
}

TEST(FaultInjection, LatencyIsApplied) {
    FaultSpec s;
    s.latency_us = 5000;  // 5ms
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    const auto t0 = std::chrono::steady_clock::now();
    bus->read(0x10, rx, sizeof(rx), &got, 1000);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(), 4000);
}

TEST(FaultInjection, WriteThenReadGetsBothPhasesAndMutations) {
    FaultSpec s;
    s.pad = 2;
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    uint8_t tx[1] = {0xAA};
    ASSERT_TRUE(bus->write_then_read(0x10, tx, 1, rx, sizeof(rx), &got));
    EXPECT_EQ(got, 6u);  // 4 frame + 2 pad
    EXPECT_EQ(rx[4], 0xFF);
}

// ---- io stats through the decorator --------------------------------------

TEST(FaultInjection, InjectedFailuresCountAsIoFailedDelegatedSuccessesAsIoOk) {
    FaultSpec s;
    s.nak_every = 2;  // every 2nd write is an injected NAK
    auto bus = wrap(s);
    uint8_t tx[1] = {0xAA};
    ASSERT_TRUE(bus->write(0x10, tx, 1));                            // 1 ok -> inner records ok
    ASSERT_EQ(bus->write(0x10, tx, 1).code, I2cError::WriteFailed);  // 2 injected -> decorator records failed
    ASSERT_TRUE(bus->write(0x10, tx, 1));                            // 3 ok
    ASSERT_EQ(bus->write(0x10, tx, 1).code, I2cError::WriteFailed);  // 4 injected
    const IoStats st = bus->io_stats_for(0x10);
    EXPECT_EQ(st.ok, 2u);      // the 2 delegated successes (from the inner bus)
    EXPECT_EQ(st.failed, 2u);  // the 2 injected NAKs (from the decorator) surface as io_failed
}

TEST(FaultInjection, MutatedButSuccessfulReadStaysIoOk) {
    FaultSpec s;
    s.pad = 4;
    s.corrupt_every = 1;  // corrupt + pad every read, but the transport succeeds
    auto bus = wrap(s);
    uint8_t rx[16] = {0};
    size_t got = 0;
    ASSERT_TRUE(bus->read(0x10, rx, sizeof(rx), &got, 1000));
    const IoStats st = bus->io_stats_for(0x10);
    EXPECT_EQ(st.ok, 1u);  // transport succeeded; corruption is a data (protocol) fault, not io
    EXPECT_EQ(st.failed, 0u);
}

// ---- io stats ------------------------------------------------------------

TEST(IoStatsMapTest, CountsOkFailedAndRetriedAttempts) {
    IoStatsMap m;
    m.record(0x10, true, 1);   // ok, no retries
    m.record(0x10, true, 3);   // ok after 2 retries
    m.record(0x10, false, 2);  // failed after 1 retry
    const IoStats s = m.stats_for(0x10);
    EXPECT_EQ(s.ok, 2u);
    EXPECT_EQ(s.failed, 1u);
    EXPECT_EQ(s.retried_attempts, 3u);    // (3-1) + (2-1)
    EXPECT_EQ(m.stats_for(0x99).ok, 0u);  // untouched address is zero
}
