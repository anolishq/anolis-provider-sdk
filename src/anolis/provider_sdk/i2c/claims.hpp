#pragma once

// The canonical claim key for an I2C address (anolis#318).
//
// Every provider on a shared bus must spell the same address the same way, or
// the runtime's exact-string uniqueness check (see claims.hpp) cannot see the
// collision. The form keeps the runtime's earlier normalization: the bus path as
// given, and a lowercase two-digit hex address, e.g. `i2c:/dev/i2c-1:0x0a`. Two
// different paths to one adapter (a symlink) are different keys, as before.

#include <string>
#include <string_view>

namespace anolis::provider_sdk::i2c {

// Throws std::invalid_argument for an empty bus path, one containing whitespace
// (a claim tag is space-separated), or an address above 0x7F (7-bit only).
std::string claim_key(std::string_view bus_path, unsigned int address);

}  // namespace anolis::provider_sdk::i2c
