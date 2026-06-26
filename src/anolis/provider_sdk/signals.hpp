#pragma once

// Static signal description + the §7.2 default-set helper (P5).
//
// `SignalDefinition` is ezo's shape (a static, per-family table of signal
// metadata). `default_ids_from` is net-new in the SDK: it derives the default
// signal-id set (returned for an empty `ReadSignalsRequest.signal_ids`) from the
// `is_default` flags, so the shared handler owns the empty→default branch
// uniformly. Providers that compute their default set another way feed a
// `SignalDefinition` span or adapt their own accessor.

#include <span>
#include <string>
#include <vector>

namespace anolis::provider_sdk {

// Static description of one device signal, shared by per-family adapter modules.
struct SignalDefinition {
    const char* signal_id;
    const char* name;
    const char* description;
    const char* unit;
    // [§7.2] Included in the default signal set returned for an empty
    // ReadSignalsRequest.signal_ids — the primary, routinely-useful telemetry.
    // Derived/specialized signals are excluded from the default.
    bool is_default;
};

// The §7.2 default signal-id set: every definition flagged `is_default`, in
// declaration order.
inline std::vector<std::string> default_ids_from(std::span<const SignalDefinition> defs) {
    std::vector<std::string> ids;
    for (const auto& def : defs) {
        if (def.is_default) {
            ids.emplace_back(def.signal_id);
        }
    }
    return ids;
}

}  // namespace anolis::provider_sdk
