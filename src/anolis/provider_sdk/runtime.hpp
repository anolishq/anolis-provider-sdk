#pragma once

// The provider-runtime seam + the generic ADPP request loop (spine, D.3b).
//
// `ProviderRuntime` is the non-templated interface the spine consumes: its
// `read`/`call` return the NEUTRAL result types (D1), so HandleT +
// DeviceAdapter<HandleT> + handle acquisition stay inside the provider's
// implementation (per #53). The spine never names HandleT. `run_loop` owns the
// §3.2 Hello-gate, the request dispatch, and the exit codes; the provider's
// main() shrinks to: parse CLI -> init -> construct its ProviderRuntime + hooks
// -> return run_loop(...). A virtual interface (not a template) is right here:
// the loop is stdio-I/O-bound, so the per-request vtable cost is irrelevant and
// the spine compiles once.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "anolis/provider_sdk/result.hpp"  // adpp alias, AdapterReadResult/AdapterCallResult, ValueMap
#include "protocol.pb.h"

namespace anolis::provider_sdk {

// Provider identity advertised in the Hello handshake.
struct ProviderMetadata {
    std::string name;                                // e.g. "anolis-provider-sim"
    std::string version;                             // provider version string
    std::string protocol_version = "v1";             // the ADPP protocol version
    std::map<std::string, std::string> hello_extra;  // extra Hello metadata keys (merged in)
};

// Startup/readiness snapshot projected into wait_ready/health responses.
struct ReadinessReport {
    struct DeviceFailure {
        std::string device_id;
        std::string type;
        std::string reason;
    };

    bool ready = true;
    int configured_device_count = 0;
    std::vector<std::string> successful_device_ids;
    std::vector<DeviceFailure> failed_devices;
    std::string startup_policy;                            // free-form, e.g. "strict"/"degraded"
    std::string provider_impl;                             // e.g. "sim"/"ezo"/"bread"
    std::map<std::string, std::string> extra_diagnostics;  // provider-specific extras
};

// Per-device health enrichment a provider may supply (SDK#9). The SDK merges it
// into each DeviceHealth it emits: `metrics` into DeviceHealth.metrics, and
// `last_seen` into the structured DeviceHealth.last_seen field only when engaged.
// Both members default to "absent" so a non-overriding provider's wire output is
// unchanged (empty map => metrics untouched; nullopt => last_seen left unset).
// Reserved metric key names and their semantics: docs/metrics.md.
struct DeviceHealthExtra {
    std::map<std::string, std::string> metrics;
    std::optional<google::protobuf::Timestamp> last_seen;
};

// The seam the generic handlers + run_loop consume. The provider implements it;
// read/call internally acquire the HandleT and dispatch through DeviceAdapter<HandleT>.
class ProviderRuntime {
public:
    virtual ~ProviderRuntime() = default;

    // Handshake / readiness / health.
    virtual ProviderMetadata metadata() const = 0;
    virtual ReadinessReport readiness() const = 0;

    // Per-device health enrichment (SDK#9). Defaulted so existing providers need
    // no change. Called once per device id the SDK surfaces in
    // list_devices(include_health) and get_health — for BOTH live-inventory ids
    // AND startup-failed/missing ids (which have no live handle, so an override
    // MUST tolerate an unknown id and return a default-constructed value). MUST be
    // in-process only (no live device I/O on this path) and SHOULD take at most one
    // internal snapshot so the returned metrics + last_seen are one atomic view.
    virtual DeviceHealthExtra device_health(const std::string& /*device_id*/) const { return {}; }

    // Inventory.
    virtual std::vector<std::string> list_device_ids() const = 0;
    virtual bool has_device(const std::string& device_id) const = 0;
    virtual adpp::Device device_info(const std::string& device_id) const = 0;
    virtual adpp::CapabilitySet capabilities(const std::string& device_id) const = 0;

    // Device-model dispatch (neutral results; handle acquisition is provider-local).
    virtual AdapterReadResult read(const std::string& device_id, const std::vector<std::string>& signal_ids) = 0;
    virtual AdapterCallResult call(const std::string& device_id, uint32_t function_id, const ValueMap& args) = 0;
    virtual std::optional<uint32_t> resolve_function_id(const std::string& device_id,
                                                        const std::string& function_name) const = 0;
};

// Provider-specific loop actions. All default to no-op; the SDK calls them at the
// right point in run_loop (sim: start/stop its physics ticker).
struct LifecycleHooks {
    std::function<void()> on_wait_ready = [] {};  // after a successful wait_ready
    std::function<void()> on_shutdown = [] {};    // on ANY exit from the loop
};

// The generic ADPP serving loop. Reads length-prefixed Request frames from
// `input`, dispatches through `runtime`, writes Response frames to `output`.
// Returns the process exit code: 0 clean EOF, 2 read error, 3 parse error,
// 4 serialize error, 5 write error. `hooks.on_shutdown()` fires on every exit.
int run_loop(std::istream& input, std::ostream& output, ProviderRuntime& runtime, const LifecycleHooks& hooks = {});

}  // namespace anolis::provider_sdk
