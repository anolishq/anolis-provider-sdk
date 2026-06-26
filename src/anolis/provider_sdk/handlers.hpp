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

// List the provider's devices (+ per-device health when include_health is set).
void handle_list_devices(const adpp::ListDevicesRequest& request, adpp::Response& response,
                         const ProviderRuntime& runtime);

// Describe one device: its metadata + full capability set.
void handle_describe_device(const adpp::DescribeDeviceRequest& request, adpp::Response& response,
                            const ProviderRuntime& runtime);

// Read signal values for one device. Owns the ADPP read policy: empty signal_ids
// -> provider default set (§7.2), unknown signal_id -> NOT_FOUND for the whole
// read (§7.4), and the §7.3 min_timestamp staleness flagging.
void handle_read_signals(const adpp::ReadSignalsRequest& request, adpp::Response& response, ProviderRuntime& runtime);

// Invoke a device function by numeric id (preferred) or resolved symbolic name.
void handle_call(const adpp::CallRequest& request, adpp::Response& response, ProviderRuntime& runtime);

// Report provider + per-device health derived from the readiness snapshot.
void handle_get_health(const adpp::GetHealthRequest& request, adpp::Response& response, const ProviderRuntime& runtime);

// Standard UNIMPLEMENTED status for unsupported operations.
void handle_unimplemented(adpp::Response& response, const std::string& message = "operation not implemented");

// [§7.3] Flag values older than ReadSignalsRequest.min_timestamp as QUALITY_STALE
// (non-fatal freshness hint). Exposed for unit testing.
void apply_min_timestamp(const adpp::ReadSignalsRequest& request, adpp::ReadSignalsResponse& out);

}  // namespace anolis::provider_sdk::handlers
