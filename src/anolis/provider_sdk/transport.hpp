#pragma once

// Length-prefixed stdio framing for the ADPP transport loop (spine).
//
// Each frame is a little-endian uint32 byte-length followed by exactly that many
// payload bytes (a serialized ADPP protobuf message). The framing layer
// guarantees all-or-nothing payload delivery so the protobuf decode above it
// never sees a partial message. Lifted verbatim from the providers' byte-
// identical framed_stdio (the algorithm was the same in all three).

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace anolis::provider_sdk::transport {

// Upper bound for one framed stdio payload accepted by the transport loop (1 MiB).
constexpr uint32_t kMaxFrameBytes = 1024u * 1024u;

// Read exactly `size` bytes or fail on EOF/stream error.
bool read_exact(std::istream& input, uint8_t* buffer, size_t size);

// Read one little-endian length-prefixed frame from the input stream.
//
// `false` with an empty `error` means a clean EOF before any new frame started
// (the caller treats it as a normal shutdown); `false` with a non-empty `error`
// is a fatal framing error.
bool read_frame(std::istream& input, std::vector<uint8_t>& out, std::string& error, uint32_t max_len = kMaxFrameBytes);

// Write one little-endian length-prefixed frame to the output stream and flush.
bool write_frame(std::ostream& output, const uint8_t* data, size_t size, std::string& error,
                 uint32_t max_len = kMaxFrameBytes);

}  // namespace anolis::provider_sdk::transport
