#pragma once

// Call-argument validation against the declared FunctionSpec (semantics.md
// §8.3). handle_call runs it before ProviderRuntime::call, so a provider's call
// sees only arguments that match the spec it declared. The rule and its order
// match the Python SDK's:
//
//   1. an argument the function does not declare  -> INVALID_ARGUMENT
//   2. a required argument that is missing         -> INVALID_ARGUMENT
//   3. a value of the wrong type                   -> INVALID_ARGUMENT
//   4. a non-finite double                         -> INVALID_ARGUMENT (before bounds)
//   5. a value outside its declared bounds         -> OUT_OF_RANGE (bounds inclusive)
//   6. a value not among allowed_values            -> INVALID_ARGUMENT

#include <optional>
#include <string>

#include "anolis/provider_sdk/result.hpp"  // adpp alias, ValueMap
#include "protocol.pb.h"

namespace anolis::provider_sdk {

struct ArgError {
    adpp::Status::Code code;
    std::string message;
};

// The first rule `args` breaks for `spec`, or nullopt when they are valid.
std::optional<ArgError> validate_call_args(const adpp::FunctionSpec& spec, const ValueMap& args);

}  // namespace anolis::provider_sdk
