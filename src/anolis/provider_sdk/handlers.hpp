#pragma once

// Generic ADPP request handlers (spine). Each fills a Response from the request
// and the ProviderRuntime seam; run_loop dispatches to them. The ADPP policy
// (§3.2 protocol gate here, §7.2/7.3/7.4 in the D.3c read/call handlers) lives in
// the SDK so every provider gets it identically.
//
// D.3b implements the handshake/lifecycle handlers below; the device-model-coupled
// handlers (list_devices/describe_device/read_signals/call/get_health) land in D.3c
// (run_loop routes them to handle_unimplemented until then).

#include <string>

#include "anolis/provider_sdk/result.hpp"  // adpp alias
#include "anolis/provider_sdk/runtime.hpp"
#include "protocol.pb.h"

namespace anolis::provider_sdk::handlers {

// Handshake: validate the protocol version and advertise provider metadata.
void handle_hello(const adpp::HelloRequest& request, adpp::Response& response, const ProviderRuntime& runtime);

// Project the readiness/startup report into wait_ready diagnostics.
void handle_wait_ready(const adpp::WaitReadyRequest& request, adpp::Response& response, const ProviderRuntime& runtime);

// Standard UNIMPLEMENTED status for unsupported operations.
void handle_unimplemented(adpp::Response& response, const std::string& message = "operation not implemented");

}  // namespace anolis::provider_sdk::handlers
