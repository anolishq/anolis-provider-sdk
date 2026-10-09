#pragma once

// Host requirement checks (anolis executable profile v1 §6, `--check-host`).
//
// A provider checks what its config needs from the host and reports each
// requirement as met, unmet or unknown. The same result is printed by
// `--check-host <config>` and, at startup, folded into readiness diagnostics so
// a provider with unmet requirements stays up and not ready instead of exiting.
// Requirement ids and their meaning are provider-owned; tooling only prints them.
// Transport-specific checks live with their transport (see i2c/host_checks.hpp).

#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace anolis::provider_sdk::host_check {

// The envelope-convention version this SDK emits.
inline constexpr int kCheckHostVersion = 1;

enum class Status {
    Met,
    Unmet,
    Unknown,  // the provider cannot tell on this host; does not fail the check
};

struct Requirement {
    std::string id;  // provider-owned, stable across releases, non-empty
    Status status = Status::Unknown;
    std::string detail;  // what was checked and found
    std::string remedy;  // for Unmet: what to change on the host
};

// "met" / "unmet" / "unknown".
const char* to_string(Status status);

// 1 when any requirement is Unmet, else 0 (the profile's exit codes; 2, "could
// not evaluate", is the provider's own answer for e.g. an invalid config).
int exit_code(const std::vector<Requirement>& requirements);

// The `--check-host` envelope as one line of JSON. `provider_name` is the
// recommended `provider` key; empty omits it. Empty `detail`/`remedy` are
// omitted. Throws std::invalid_argument for a requirement with an empty id.
std::string envelope(const std::string& provider_name, const std::vector<Requirement>& requirements);

// Writes the envelope plus a newline and returns exit_code(): everything a
// provider's `--check-host` verb prints to stdout, and what it exits with.
int write_envelope(std::ostream& out, const std::string& provider_name, const std::vector<Requirement>& requirements);

// The readiness diagnostics for a startup check, to merge into
// ReadinessReport::extra_diagnostics: `host_check` is "ok" or "unmet", and
// `host_unmet` (only when unmet) summarizes the unmet ids and details.
std::map<std::string, std::string> readiness_diagnostics(const std::vector<Requirement>& requirements);

}  // namespace anolis::provider_sdk::host_check
