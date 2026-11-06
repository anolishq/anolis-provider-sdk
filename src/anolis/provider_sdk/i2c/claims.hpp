#pragma once

// Claim keys for an I2C address (anolis#318, #47).
//
// Every provider on a shared bus must spell the same address the same way, or
// the runtime's exact-string uniqueness check (see
// anolis/provider_sdk/claims.hpp) cannot see the collision. A key is
// `i2c:<bus>:<address>`, the address as lowercase two-digit hex, e.g.
// `i2c:/dev/i2c-1:0x0a`.
//
// A provider publishes every key from claim_keys, for example
//
//     for (const auto& key : i2c::claim_keys(bus_path, address)) {
//         add_claim(device, key);
//     }
//
// claim_keys gives up to two keys:
//
// 1. The bus path as configured. This is the key every earlier SDK release
//    published, so a provider on this release still collides with one on an
//    older release that configured the same string.
// 2. The bus path in one spelling per adapter, when that differs from (1), so
//    that two paths to one adapter share a key:
//    - A path with a scheme (`mock://bench`, anything containing `://`) names
//      no filesystem node and has no second key.
//    - Otherwise the path is resolved as realpath(3) would, at the moment
//      claim_keys is called. When every component of the path exists, symlinks
//      (a udev alias such as `/dev/i2c-sensors`), `.` and `..` segments, and
//      repeated or trailing separators all collapse onto the node, and a
//      relative path becomes absolute.
//    - When a component does not exist (the node is not there yet, or an
//      earlier directory is missing), the existing leading part is resolved and
//      the rest is normalised lexically, so `/dev//i2c-9` gives `/dev/i2c-9`.
//      Such a key stays relative if the first component is missing (e.g.
//      `nothere/../dev/i2c-1` gives `dev/i2c-1`), and an alias of a missing node
//      is caught only if it normalises to the same string. The same lexical
//      fallback applies when resolving fails: a symlink loop, or a directory
//      the provider cannot search, so two providers running as different users
//      can resolve one path differently. Nothing reports that a key fell back;
//      claim after the bus is present, and configure absolute paths.
//    - On Windows, which has no i2c-dev, only the lexical normalisation applies.
//
// Two providers collide when their key sets share a key. For absolute paths,
// publishing both keys adds no false collision: an equal configured string, or
// an equal resolved path, names the same node. Aliases of one adapter are caught when both
// providers publish claim_keys; a provider on an older SDK publishes only (1),
// so an alias it uses is caught only if it equals the other's (1) or (2).
//
// claim_key gives key (1) alone: the 0.6.0 behaviour, unchanged.

#include <string>
#include <string_view>
#include <vector>

namespace anolis::provider_sdk::i2c {

// The key for the bus path as configured (key 1 above). Throws
// std::invalid_argument for an empty bus path, one containing whitespace (a
// claim tag is space-separated), or an address above 0x7F (7-bit only).
std::string claim_key(std::string_view bus_path, unsigned int address);

// The keys to publish for an address: the configured key, then the canonical
// key when it differs. Throws as claim_key does, and also when the path
// resolves to a name containing whitespace.
std::vector<std::string> claim_keys(std::string_view bus_path, unsigned int address);

}  // namespace anolis::provider_sdk::i2c
