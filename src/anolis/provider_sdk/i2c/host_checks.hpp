#pragma once

// Reusable host checks for a Linux i2c-dev bus, for a provider's `--check-host`
// and its startup readiness (executable profile v1 §6). Read-only: they inspect
// the device node and sysfs, and never talk to the hardware on the bus.
//
// Requirement ids (stable):
// - `i2c.bus_present` — the bus's device node exists.
// - `i2c.bus_access`  — the current user can open it read-write. Denied: the
//   detail names the node's owning group. Run as root it is `unknown`, because
//   root opens anything and the answer would say nothing about the user the
//   runtime runs as; run the check as that user.
// - `i2c.bus_clock`   — only with a maximum: the adapter's configured
//   `clock-frequency` from the device tree is at most that maximum. `unknown`
//   where the platform does not expose it. The configured rate is a ceiling:
//   a platform that scales the bus's source clock may run it slower.
//
// When the node is missing the dependent checks are `unknown`, so the same ids
// are reported on every run.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "anolis/provider_sdk/host_check.hpp"

namespace anolis::provider_sdk::i2c {

struct HostCheckOptions {
    // The highest bus clock this provider's config allows, in Hz. Unset skips
    // `i2c.bus_clock`.
    std::optional<uint32_t> max_bus_hz;
    // Where sysfs is mounted; tests point it at a fixture tree.
    std::string sysfs_root = "/sys";
};

std::vector<host_check::Requirement> check_host(const std::string& bus_path, const HostCheckOptions& options = {});

}  // namespace anolis::provider_sdk::i2c
