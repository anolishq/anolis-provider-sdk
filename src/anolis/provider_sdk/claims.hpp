#pragma once

// Opaque resource claims (anolis#318).
//
// A device declares the resources it needs exclusively as keys in its
// `anolis.claim` descriptor tag, space-separated when there are several. The
// runtime rejects two devices that publish the same key, comparing exact strings
// and parsing nothing, so it knows no transport. Two providers sharing a resource
// must therefore spell its key identically: the key's format belongs to whoever
// defines the resource, e.g. `i2c::claim_key` for an address on an I2C bus.

#include <string_view>

#include "anolis/provider_sdk/result.hpp"  // adpp alias

namespace anolis::provider_sdk {

// The descriptor tag the runtime reads claims from.
inline constexpr std::string_view kClaimTag = "anolis.claim";

// Adds `key` to `device`'s `anolis.claim` tag, keeping any keys already there; a
// key already present is not repeated. Throws std::invalid_argument for an empty
// key or one containing whitespace, which would split into several claims.
void add_claim(adpp::Device& device, std::string_view key);

}  // namespace anolis::provider_sdk
