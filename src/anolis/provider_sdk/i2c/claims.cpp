#include "anolis/provider_sdk/i2c/claims.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace anolis::provider_sdk::i2c {

namespace {

bool has_whitespace(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}

// One spelling per adapter: see claims.hpp for the rules.
std::string canonical_bus_path(std::string_view bus_path) {
    if (bus_path.find("://") != std::string_view::npos) {
        return std::string(bus_path);  // a scheme (`mock://...`) names no filesystem node
    }
    const std::filesystem::path given{std::string(bus_path)};
    std::filesystem::path resolved;
#ifndef _WIN32
    // realpath(3) on the part of the path that exists, lexical for the rest.
    std::error_code ec;
    resolved = std::filesystem::weakly_canonical(given, ec);
    if (ec) {
        resolved.clear();
    }
#endif
    if (resolved.empty()) {
        resolved = given.lexically_normal();
    }
    std::string out = resolved.generic_string();
    // `/dev/i2c-1/` and `/dev/i2c-1` name the same node; lexical normal form keeps the slash.
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

}  // namespace

std::string claim_key(std::string_view bus_path, unsigned int address) {
    if (bus_path.empty() || has_whitespace(bus_path)) {
        throw std::invalid_argument("I2C claim key: bus path must be non-empty and contain no whitespace: '" +
                                    std::string(bus_path) + "'");
    }
    if (address > 0x7FU) {
        throw std::invalid_argument("I2C claim key: address " + std::to_string(address) + " is not a 7-bit address");
    }
    const std::string path = canonical_bus_path(bus_path);
    if (has_whitespace(path)) {
        // A symlink can resolve to a name with a space, which would split the claim tag.
        throw std::invalid_argument("I2C claim key: bus path '" + std::string(bus_path) + "' resolves to '" + path +
                                    "', which contains whitespace");
    }
    char hex[8];
    std::snprintf(hex, sizeof(hex), "0x%02x", address);
    return "i2c:" + path + ":" + hex;
}

}  // namespace anolis::provider_sdk::i2c
