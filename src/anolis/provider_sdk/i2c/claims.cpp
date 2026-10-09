#include "anolis/provider_sdk/i2c/claims.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace anolis::provider_sdk::i2c {

std::string claim_key(std::string_view bus_path, unsigned int address) {
    const bool has_space = std::any_of(bus_path.begin(), bus_path.end(),
                                       [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
    if (bus_path.empty() || has_space) {
        throw std::invalid_argument("I2C claim key: bus path must be non-empty and contain no whitespace: '" +
                                    std::string(bus_path) + "'");
    }
    if (address > 0x7FU) {
        throw std::invalid_argument("I2C claim key: address " + std::to_string(address) + " is not a 7-bit address");
    }
    char hex[8];
    std::snprintf(hex, sizeof(hex), "0x%02x", address);
    return "i2c:" + std::string(bus_path) + ":" + hex;
}

}  // namespace anolis::provider_sdk::i2c
