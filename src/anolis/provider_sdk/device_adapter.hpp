#pragma once

// DeviceAdapter<HandleT> — the header-only device descriptor templated over the
// provider's hardware-session handle (D3 / anolis-protocol#53).
//
// `HandleT` is a plain template type parameter (not type-erasure): each provider
// keeps its real session type with zero virtual indirection —
//   bread: HandleT = crumbs::Session    ezo: HandleT = EzoHandle    sim: HandleT = std::monostate
// The handle is acquired provider-locally BEFORE read/call run (ezo binds inside
// its bus executor; bread passes the runtime session; sim passes monostate{}).
//
// `read_signals` and `call` are the REQUIRED common core, both returning neutral
// results (result.hpp). `get_capabilities` / `get_device_info` are OPTIONAL
// (nullable): null when the provider computes them in core / on a device record
// (ezo, bread), set when the provider owns them on the descriptor (sim).
//
// The descriptor instances stay file-static PODs of function pointers — exactly
// the providers' current shape, just templated. The per-provider taxonomy
// (`enum class <P>DeviceType` + `-Werror=switch` `adapter_for`) stays provider-
// local; the SDK never names a provider's device types.

#include <cstdint>
#include <string>
#include <vector>

#include "anolis/provider_sdk/device_spec.hpp"
#include "anolis/provider_sdk/result.hpp"  // adpp alias, AdapterReadResult/AdapterCallResult, ValueMap
#include "protocol.pb.h"

namespace anolis::provider_sdk {

template <typename HandleT>
struct DeviceAdapter {
    // REQUIRED — the common core; HandleT threads through by reference.
    AdapterReadResult (*read_signals)(HandleT& handle, const DeviceSpec& device,
                                      const std::vector<std::string>& signal_ids) = nullptr;
    AdapterCallResult (*call)(HandleT& handle, const DeviceSpec& device, uint32_t function_id,
                              const ValueMap& args) = nullptr;

    // OPTIONAL (nullable) — null when the provider computes these in core / on the
    // device record (ezo, bread); set when it owns them on the descriptor (sim).
    adpp::CapabilitySet (*get_capabilities)() = nullptr;
    adpp::Device (*get_device_info)(const DeviceSpec& device) = nullptr;
};

}  // namespace anolis::provider_sdk
