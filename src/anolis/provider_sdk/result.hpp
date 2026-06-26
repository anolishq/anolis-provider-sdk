#pragma once

// Neutral, ADPP-typed results returned by device read/call execution (D1).
//
// `AdapterReadResult` wraps the read's `SignalValue`s with an explicit status
// channel; `AdapterCallResult` is the status-only call result. This is the shape
// ezo and bread already share byte-for-byte; sim's `CallResult{code,message}`
// folds onto `AdapterCallResult` via the `call_*` factories below. Carrying the
// proto `Status::Code` here means the request handler never maps a provider-local
// status vocabulary (the 1-stage error map). Depends only on the ADPP proto.

#include <google/protobuf/map.h>

#include <string>
#include <vector>

#include "protocol.pb.h"

namespace anolis::provider_sdk {

// Short alias for the generated ADPP v1 protobuf namespace, used throughout the
// SDK device-model headers.
namespace adpp = ::anolis::deviceprovider::v1;

// Protobuf argument map type carried by device function calls (the wire type —
// the same `Map` that lives on the request, so no copy at the call boundary).
using ValueMap = google::protobuf::Map<std::string, adpp::Value>;

// Normalized result of one adapter read. `ok == false` means the adapter could
// not produce a coherent signal set; `error_code`/`error_message` are then
// already mapped into ADPP semantics for the caller.
struct AdapterReadResult {
    bool ok = false;
    adpp::Status::Code error_code = adpp::Status::CODE_UNAVAILABLE;
    std::string error_message;
    std::vector<adpp::SignalValue> values;
};

// Normalized result of one adapter function call (status only). An optional
// per-function results payload is a tracked follow-up (anolis-protocol#57).
struct AdapterCallResult {
    bool ok = false;
    adpp::Status::Code error_code = adpp::Status::CODE_UNAVAILABLE;
    std::string error_message;
};

// Call-result factories — the field-for-field fold of sim's `CallResult`
// vocabulary (`ok`/`bad`/`nf`/`precond`/`out_of_range`) onto the neutral
// `AdapterCallResult`. `ok` is implied by a non-error code.
inline AdapterCallResult call_ok() { return {true, adpp::Status::CODE_OK, "ok"}; }

inline AdapterCallResult call_invalid_argument(const std::string& message) {
    return {false, adpp::Status::CODE_INVALID_ARGUMENT, message};
}

inline AdapterCallResult call_not_found(const std::string& message) {
    return {false, adpp::Status::CODE_NOT_FOUND, message};
}

inline AdapterCallResult call_failed_precondition(const std::string& message) {
    return {false, adpp::Status::CODE_FAILED_PRECONDITION, message};
}

inline AdapterCallResult call_out_of_range(const std::string& message) {
    return {false, adpp::Status::CODE_OUT_OF_RANGE, message};
}

inline AdapterCallResult call_unavailable(const std::string& message) {
    return {false, adpp::Status::CODE_UNAVAILABLE, message};
}

}  // namespace anolis::provider_sdk
