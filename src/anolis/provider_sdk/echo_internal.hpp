#pragma once

// Internal to the spine's translation units (handlers.cpp, call_validation.cpp)
// — NOT part of the public SDK API. One rule for quoting a request field in an
// error message, so every error reply stays small however large the field is
// (#50).

#include <cstddef>
#include <string>

namespace anolis::provider_sdk::internal {

// At most this many bytes of a request field are quoted. The Python SDK's
// limit is 64 characters.
inline constexpr std::size_t kMaxEchoBytes = 64;

// `field` cut to kMaxEchoBytes on a UTF-8 character boundary, with `...`
// marking the cut; a shorter field is returned unchanged.
inline std::string echo(const std::string& field) {
    if (field.size() <= kMaxEchoBytes) {
        return field;
    }
    std::size_t end = kMaxEchoBytes;
    while (end > 0 && (static_cast<unsigned char>(field[end]) & 0xC0U) == 0x80U) {
        --end;  // field[end] continues the character before it: cut before that character
    }
    return field.substr(0, end) + "...";
}

}  // namespace anolis::provider_sdk::internal
