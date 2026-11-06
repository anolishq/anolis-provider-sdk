#pragma once

// The canonical claim key for an I2C address (anolis#318).
//
// Every provider on a shared bus must spell the same address the same way, or
// the runtime's exact-string uniqueness check (see
// anolis/provider_sdk/claims.hpp) cannot see the collision. The key is
// `i2c:<bus>:<address>`, the address as lowercase two-digit hex, e.g.
// `i2c:/dev/i2c-1:0x0a`.
//
// `<bus>` is the bus path in one spelling per adapter, so that two paths to one
// adapter give one key (#47):
//
// - A path with a scheme (`mock://bench`) names no filesystem node and is used
//   exactly as given.
// - Otherwise the path is resolved as realpath(3) would, at the moment
//   claim_key is called: symlinks (a udev alias such as `/dev/i2c-sensors`),
//   `.` and `..` segments, and repeated or trailing separators all collapse
//   onto the node they name, and a relative path to an existing node becomes
//   absolute.
// - When the node does not exist (yet), the part of the path that does exist
//   is resolved and the rest is normalised lexically, so `/dev//i2c-9` gives
//   `/dev/i2c-9`. An alias of a missing node is then caught only if it
//   normalises to the same string; a provider should claim after the bus is
//   present. The same lexical fallback applies when resolving fails (for
//   example a symlink loop or a directory the provider cannot search).
// - On Windows, which has no i2c-dev, only the lexical normalisation applies.
//
// The runtime still compares keys as opaque strings, so a collision is seen
// only when every provider involved builds its key with this version or later.

#include <string>
#include <string_view>

namespace anolis::provider_sdk::i2c {

// Throws std::invalid_argument for an empty bus path, one containing whitespace
// (a claim tag is space-separated), one that resolves to a path containing
// whitespace, or an address above 0x7F (7-bit only).
std::string claim_key(std::string_view bus_path, unsigned int address);

}  // namespace anolis::provider_sdk::i2c
