#include "anolis/provider_sdk/claims.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>

namespace anolis::provider_sdk {

void add_claim(adpp::Device& device, std::string_view key) {
    const bool has_space =
        std::any_of(key.begin(), key.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
    if (key.empty() || has_space) {
        throw std::invalid_argument("claim key must be non-empty and contain no whitespace: '" + std::string(key) +
                                    "'");
    }

    auto& tags = *device.mutable_tags();
    std::string& value = tags[std::string(kClaimTag)];
    std::istringstream existing(value);
    std::string token;
    while (existing >> token) {
        if (token == key) {
            return;
        }
    }
    if (!value.empty()) {
        value += ' ';
    }
    value += key;
}

}  // namespace anolis::provider_sdk
