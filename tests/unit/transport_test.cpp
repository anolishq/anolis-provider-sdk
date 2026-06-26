#include "anolis/provider_sdk/transport.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

// D.3a framed-stdio transport tests: round-trip, the clean-EOF vs fatal-error
// distinction, and the length guards.

namespace tx = anolis::provider_sdk::transport;

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST(TransportTest, WriteThenReadRoundTrips) {
    const auto payload = bytes("hello adpp frame");
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);

    std::string err;
    ASSERT_TRUE(tx::write_frame(stream, payload.data(), payload.size(), err)) << err;
    EXPECT_TRUE(err.empty());

    std::vector<uint8_t> got;
    ASSERT_TRUE(tx::read_frame(stream, got, err)) << err;
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(got, payload);
}

TEST(TransportTest, TwoFramesReadBackInOrder) {
    const auto a = bytes("first");
    const auto b = bytes("second");
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    std::string err;
    ASSERT_TRUE(tx::write_frame(stream, a.data(), a.size(), err)) << err;
    ASSERT_TRUE(tx::write_frame(stream, b.data(), b.size(), err)) << err;

    std::vector<uint8_t> got;
    ASSERT_TRUE(tx::read_frame(stream, got, err));
    EXPECT_EQ(got, a);
    ASSERT_TRUE(tx::read_frame(stream, got, err));
    EXPECT_EQ(got, b);
}

TEST(TransportTest, CleanEofReturnsFalseWithEmptyError) {
    std::stringstream empty(std::ios::in | std::ios::out | std::ios::binary);
    std::vector<uint8_t> got;
    std::string err = "sentinel";
    EXPECT_FALSE(tx::read_frame(empty, got, err));
    EXPECT_TRUE(err.empty()) << "clean EOF must clear the error (normal shutdown)";
}

TEST(TransportTest, TruncatedHeaderIsFatal) {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    const uint8_t partial_header[2] = {0x04, 0x00};  // 1st byte present, header incomplete
    stream.write(reinterpret_cast<const char*>(partial_header), 2);

    std::vector<uint8_t> got;
    std::string err;
    EXPECT_FALSE(tx::read_frame(stream, got, err));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("header"), std::string::npos);
}

TEST(TransportTest, TruncatedPayloadIsFatal) {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    const uint8_t header[4] = {0x08, 0x00, 0x00, 0x00};  // claims 8 bytes
    stream.write(reinterpret_cast<const char*>(header), 4);
    const auto partial = bytes("abc");  // only 3 provided
    stream.write(reinterpret_cast<const char*>(partial.data()), static_cast<std::streamsize>(partial.size()));

    std::vector<uint8_t> got;
    std::string err;
    EXPECT_FALSE(tx::read_frame(stream, got, err));
    EXPECT_NE(err.find("payload"), std::string::npos);
}

TEST(TransportTest, ZeroLengthIsRejectedBothWays) {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    std::string err;
    EXPECT_FALSE(tx::write_frame(stream, nullptr, 0, err));
    EXPECT_FALSE(err.empty());

    const uint8_t zero_header[4] = {0, 0, 0, 0};
    stream.write(reinterpret_cast<const char*>(zero_header), 4);
    std::vector<uint8_t> got;
    EXPECT_FALSE(tx::read_frame(stream, got, err));
    EXPECT_FALSE(err.empty());
}

TEST(TransportTest, OversizeIsRejectedAgainstMaxLen) {
    const auto payload = bytes("0123456789");  // 10 bytes
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    std::string err;
    EXPECT_FALSE(tx::write_frame(stream, payload.data(), payload.size(), err, /*max_len=*/4));
    EXPECT_NE(err.find("max"), std::string::npos);

    // A header that advertises a length above max_len is rejected on read.
    std::stringstream rstream(std::ios::in | std::ios::out | std::ios::binary);
    const uint8_t big_header[4] = {0x05, 0x00, 0x00, 0x00};  // 5 > max_len 4
    rstream.write(reinterpret_cast<const char*>(big_header), 4);
    std::vector<uint8_t> got;
    EXPECT_FALSE(tx::read_frame(rstream, got, err, /*max_len=*/4));
    EXPECT_NE(err.find("max"), std::string::npos);
}

TEST(TransportTest, ReadExactSucceedsAndFails) {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    const auto data = bytes("ABCD");
    stream.write(reinterpret_cast<const char*>(data.data()), 4);

    uint8_t buf[4] = {0};
    EXPECT_TRUE(tx::read_exact(stream, buf, 4));
    EXPECT_EQ(std::string(buf, buf + 4), "ABCD");
    EXPECT_FALSE(tx::read_exact(stream, buf, 1)) << "no bytes left -> false";
}
