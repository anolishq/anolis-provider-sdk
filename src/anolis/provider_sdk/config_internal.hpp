#pragma once

// Internal to the config-schema toolkit's translation units (config.cpp /
// config_validate.cpp) — NOT part of the public SDK API. Shared constants so
// the emitted schema and the validator cannot disagree about the built-in
// I2cAddress scalar.

#include <cstdint>

namespace anolis::provider_sdk::config::internal {

inline constexpr std::int64_t kI2cAddressMin = 0x08;
inline constexpr std::int64_t kI2cAddressMax = 0x77;
// Exactly the 0x08-0x77 range so the emitted schema's string branch is as
// honest as the integer branch: 0x08-0x0F | 0x10-0x6F | 0x70-0x77.
inline constexpr const char* kI2cHexPattern = "^0[xX](0[89a-fA-F]|[1-6][0-9a-fA-F]|7[0-7])$";

}  // namespace anolis::provider_sdk::config::internal
