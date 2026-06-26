#pragma once

// The spine device identity passed to a `DeviceAdapter<HandleT>` (G3).
//
// Composition, not a typed union: the SDK spec carries only the provider-neutral
// identity — `id`, `label`, `type` (the type string each provider parses into
// its own closed enum). Provider-typed addressing (ezo/bread's numeric I2C
// address) does NOT live here; it rides the per-call `HandleT` bundle, so there
// is no downcast and a narrow typed spine never breaks sim's free-form devices.
//
// The opaque per-device config subtree (`extra`) that sim's free-form device
// schemas consume is added with the config-toolkit lift (D.3 / anolis-protocol#44),
// where it is actually parsed — keeping this device-model header free of a YAML
// dependency. Adding it then is purely additive (no provider consumes `DeviceSpec`
// yet).

#include <string>

namespace anolis::provider_sdk {

struct DeviceSpec {
    std::string id;     // stable device identifier (the registry/handler key)
    std::string label;  // human-readable label
    std::string type;   // device-type string; the provider parses it to its enum
};

}  // namespace anolis::provider_sdk
