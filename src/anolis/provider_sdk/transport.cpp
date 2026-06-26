#include "anolis/provider_sdk/transport.hpp"

// Blocking framed stdio helpers used by the provider's ADPP request loop.

namespace anolis::provider_sdk::transport {
namespace {

uint32_t decode_u32_le(const uint8_t bytes[4]) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void encode_u32_le(uint32_t value, uint8_t bytes[4]) {
    bytes[0] = static_cast<uint8_t>(value & 0xFFu);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    bytes[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

}  // namespace

bool read_exact(std::istream& input, uint8_t* buffer, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        input.read(reinterpret_cast<char*>(buffer + offset), static_cast<std::streamsize>(size - offset));
        const std::streamsize count = input.gcount();

        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }

        return false;
    }
    return true;
}

bool read_frame(std::istream& input, std::vector<uint8_t>& out, std::string& error, uint32_t max_len) {
    error.clear();

    uint8_t header[4] = {0, 0, 0, 0};

    input.read(reinterpret_cast<char*>(header), 1);
    if (input.gcount() == 0) {
        // EOF before the next header starts is treated as a clean shutdown by
        // the caller rather than as a framing error.
        return false;
    }
    if (!read_exact(input, header + 1, 3)) {
        error = "unexpected EOF while reading frame header";
        return false;
    }

    const uint32_t size = decode_u32_le(header);
    if (size == 0) {
        error = "invalid frame length: 0";
        return false;
    }
    if (size > max_len) {
        error = "frame length exceeds max";
        return false;
    }

    out.assign(size, 0);
    // The framing layer guarantees all-or-nothing payload delivery so protobuf
    // decoding above it never sees partial ADPP messages.
    if (!read_exact(input, out.data(), size)) {
        error = "unexpected EOF while reading frame payload";
        return false;
    }

    return true;
}

bool write_frame(std::ostream& output, const uint8_t* data, size_t size, std::string& error, uint32_t max_len) {
    error.clear();

    if (size == 0) {
        error = "invalid frame length: 0";
        return false;
    }
    if (size > max_len) {
        error = "frame length exceeds max";
        return false;
    }
    if (size > 0xFFFFFFFFu) {
        error = "frame length exceeds uint32";
        return false;
    }

    uint8_t header[4];
    encode_u32_le(static_cast<uint32_t>(size), header);

    output.write(reinterpret_cast<const char*>(header), 4);
    if (!output.good()) {
        error = "failed writing frame header";
        return false;
    }

    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output.good()) {
        error = "failed writing frame payload";
        return false;
    }

    output.flush();
    if (!output.good()) {
        error = "failed flushing output";
        return false;
    }

    return true;
}

}  // namespace anolis::provider_sdk::transport
