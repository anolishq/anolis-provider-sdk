#include "anolis/provider_sdk/handlers.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "anolis/provider_sdk/transport.hpp"  // kMaxFrameBytes for the Hello metadata
#include "anolis/provider_sdk/values.hpp"     // make_bool_val for the call `accepted` result

namespace anolis::provider_sdk::handlers {
namespace {

void set_status(adpp::Response& response, adpp::Status::Code code, const std::string& message) {
    response.mutable_status()->set_code(code);
    response.mutable_status()->set_message(message);
}

void set_status_ok(adpp::Response& response) { set_status(response, adpp::Status::CODE_OK, "ok"); }

// Provider health derived from the readiness snapshot: DEGRADED if any configured
// device failed to initialize, else OK. (Lifted from sim's make_provider_health;
// the SDK owns it so every provider reports health identically.)
adpp::ProviderHealth make_provider_health(const ReadinessReport& report) {
    adpp::ProviderHealth health;
    const std::size_t initialized = report.successful_device_ids.size();
    const std::size_t inferred_failed = report.configured_device_count > static_cast<int>(initialized)
                                            ? static_cast<std::size_t>(report.configured_device_count) - initialized
                                            : 0U;
    const std::size_t failed =
        report.failed_devices.size() > inferred_failed ? report.failed_devices.size() : inferred_failed;

    if (failed > 0U) {
        health.set_state(adpp::ProviderHealth::STATE_DEGRADED);
        health.set_message("startup degraded: " + std::to_string(failed) + " of " +
                           std::to_string(report.configured_device_count) + " devices failed to initialize");
    } else {
        health.set_state(adpp::ProviderHealth::STATE_OK);
        health.set_message("ok");
    }

    auto& metrics = *health.mutable_metrics();
    metrics["impl"] = report.provider_impl;
    metrics["startup_policy"] = report.startup_policy;
    metrics["startup_configured_devices"] = std::to_string(report.configured_device_count);
    metrics["startup_initialized_devices"] = std::to_string(initialized);
    metrics["startup_failed_devices"] = std::to_string(failed);
    return health;
}

// Per-device health for `device_ids`, cross-referenced with the readiness
// snapshot: UNREACHABLE (with the failure reason) for ids that failed to
// initialize, OK otherwise. The caller passes the device set to cover — the LIVE
// inventory, not just the startup report — because a provider can expose live
// devices that were never in the startup report (e.g. a synthetic control
// channel), and inventory.proto requires list_devices(include_health) to carry a
// health entry for every listed device.
std::vector<adpp::DeviceHealth> make_device_health(const std::vector<std::string>& device_ids,
                                                   const ReadinessReport& report) {
    std::unordered_map<std::string, const ReadinessReport::DeviceFailure*> failed;
    for (const auto& failure : report.failed_devices) {
        failed.emplace(failure.device_id, &failure);
    }
    std::vector<adpp::DeviceHealth> out;
    out.reserve(device_ids.size());
    for (const auto& id : device_ids) {
        adpp::DeviceHealth dh;
        dh.set_device_id(id);
        const auto it = failed.find(id);
        if (it != failed.end()) {
            // A device that failed to initialize couldn't be brought up/reached —
            // UNREACHABLE is the accurate state (FAULT is for a device that came up
            // but reported an internal fault).
            dh.set_state(adpp::DeviceHealth::STATE_UNREACHABLE);
            dh.set_message(it->second->reason);
        } else {
            dh.set_state(adpp::DeviceHealth::STATE_OK);
            dh.set_message("ok");
        }
        out.push_back(std::move(dh));
    }
    return out;
}

}  // namespace

void handle_hello(const adpp::HelloRequest& request, adpp::Response& response, const ProviderRuntime& runtime) {
    const ProviderMetadata meta = runtime.metadata();

    if (request.protocol_version() != meta.protocol_version) {
        set_status(response, adpp::Status::CODE_FAILED_PRECONDITION,
                   "unsupported protocol_version; expected " + meta.protocol_version);
        return;
    }

    auto* hello = response.mutable_hello();
    hello->set_protocol_version(meta.protocol_version);
    hello->set_provider_name(meta.name);
    hello->set_provider_version(meta.version);

    auto& md = *hello->mutable_metadata();
    md["transport"] = "stdio+uint32_le";
    md["max_frame_bytes"] = std::to_string(transport::kMaxFrameBytes);
    for (const auto& [key, value] : meta.hello_extra) {
        md[key] = value;
    }

    set_status_ok(response);
}

void handle_wait_ready(const adpp::WaitReadyRequest& /*request*/, adpp::Response& response,
                       const ProviderRuntime& runtime) {
    const ReadinessReport report = runtime.readiness();
    const ProviderMetadata meta = runtime.metadata();

    auto& diag = *response.mutable_wait_ready()->mutable_diagnostics();
    diag["device_count"] = std::to_string(report.successful_device_ids.size());
    diag["startup_policy"] = report.startup_policy;
    diag["startup_configured_devices"] = std::to_string(report.configured_device_count);
    diag["startup_initialized_devices"] = std::to_string(report.successful_device_ids.size());
    diag["startup_failed_devices"] = std::to_string(report.failed_devices.size());
    diag["startup_degraded"] = report.failed_devices.empty() ? "false" : "true";
    diag["provider_version"] = meta.version;
    diag["provider_impl"] = report.provider_impl;
    for (const auto& [key, value] : report.extra_diagnostics) {
        diag[key] = value;
    }

    set_status_ok(response);
}

void handle_list_devices(const adpp::ListDevicesRequest& request, adpp::Response& response,
                         const ProviderRuntime& runtime) {
    auto* out = response.mutable_list_devices();
    const auto device_ids = runtime.list_device_ids();
    for (const auto& id : device_ids) {
        *out->add_devices() = runtime.device_info(id);
    }
    if (request.include_health()) {
        // Cover exactly the listed devices (the harness rejects health for any
        // device not in the inventory, and requires one entry per listed device).
        for (auto& health : make_device_health(device_ids, runtime.readiness())) {
            *out->add_device_health() = std::move(health);
        }
    }
    set_status_ok(response);
}

void handle_describe_device(const adpp::DescribeDeviceRequest& request, adpp::Response& response,
                            const ProviderRuntime& runtime) {
    if (request.device_id().empty()) {
        set_status(response, adpp::Status::CODE_INVALID_ARGUMENT, "device_id is required");
        return;
    }
    if (!runtime.has_device(request.device_id())) {
        set_status(response, adpp::Status::CODE_NOT_FOUND, "unknown device_id: " + request.device_id());
        return;
    }
    auto* out = response.mutable_describe_device();
    *out->mutable_device() = runtime.device_info(request.device_id());
    *out->mutable_capabilities() = runtime.capabilities(request.device_id());
    set_status_ok(response);
}

void apply_min_timestamp(const adpp::ReadSignalsRequest& request, adpp::ReadSignalsResponse& out) {
    if (!request.has_min_timestamp()) {
        return;
    }
    const auto& min_ts = request.min_timestamp();
    for (auto& value : *out.mutable_values()) {
        const auto& ts = value.timestamp();
        const bool older =
            ts.seconds() < min_ts.seconds() || (ts.seconds() == min_ts.seconds() && ts.nanos() < min_ts.nanos());
        if (older) {
            value.set_quality(adpp::SignalValue::QUALITY_STALE);
        }
    }
}

void handle_read_signals(const adpp::ReadSignalsRequest& request, adpp::Response& response, ProviderRuntime& runtime) {
    if (request.device_id().empty()) {
        set_status(response, adpp::Status::CODE_INVALID_ARGUMENT, "device_id is required");
        return;
    }
    if (!runtime.has_device(request.device_id())) {
        set_status(response, adpp::Status::CODE_NOT_FOUND, "unknown device_id: " + request.device_id());
        return;
    }

    std::vector<std::string> ids(request.signal_ids().begin(), request.signal_ids().end());

    // §7.4: choose ONE consistent unknown-signal policy. Fail the whole read with
    // NOT_FOUND if ANY explicitly requested id is unknown (never partial). Empty
    // signal_ids means the default set (§7.2) and skips this check.
    if (!ids.empty()) {
        const auto caps = runtime.capabilities(request.device_id());
        std::unordered_set<std::string> known;
        known.reserve(static_cast<std::size_t>(caps.signals_size()));
        for (const auto& sig : caps.signals()) {
            known.insert(sig.signal_id());
        }
        for (const auto& id : ids) {
            if (known.find(id) == known.end()) {
                set_status(response, adpp::Status::CODE_NOT_FOUND,
                           "unknown signal_id '" + id + "' for device '" + request.device_id() + "'");
                return;
            }
        }
    }

    const AdapterReadResult result = runtime.read(request.device_id(), ids);
    if (!result.ok) {
        set_status(response, result.error_code, result.error_message);
        return;
    }

    auto* out = response.mutable_read_signals();
    out->set_device_id(request.device_id());
    for (const auto& value : result.values) {
        *out->add_values() = value;
    }
    apply_min_timestamp(request, *out);
    set_status_ok(response);
}

void handle_call(const adpp::CallRequest& request, adpp::Response& response, ProviderRuntime& runtime) {
    if (request.device_id().empty()) {
        set_status(response, adpp::Status::CODE_INVALID_ARGUMENT, "device_id is required");
        return;
    }
    if (request.function_id() == 0 && request.function_name().empty()) {
        set_status(response, adpp::Status::CODE_INVALID_ARGUMENT, "function_id or function_name is required");
        return;
    }

    // §6.2: prefer function_id when both are set; only resolve the name when unset.
    uint32_t resolved_function_id = request.function_id();
    if (request.function_id() == 0) {
        const auto resolved = runtime.resolve_function_id(request.device_id(), request.function_name());
        if (!resolved.has_value()) {
            set_status(
                response, adpp::Status::CODE_NOT_FOUND,
                "unknown function_name '" + request.function_name() + "' for device_id '" + request.device_id() + "'");
            return;
        }
        resolved_function_id = *resolved;
    }

    // CallRequest.args is already a ValueMap (the wire type) — pass it through.
    const AdapterCallResult result = runtime.call(request.device_id(), resolved_function_id, request.args());
    if (!result.ok) {
        set_status(response, result.error_code, result.error_message);
        return;
    }

    auto* out = response.mutable_call();
    out->set_device_id(request.device_id());
    // §8: stamp the declared `accepted` result on success. A richer per-function
    // results payload is the AdapterCallResult follow-up (anolis-protocol#57).
    (*out->mutable_results())["accepted"] = make_bool_val(true);
    set_status_ok(response);
}

void handle_get_health(const adpp::GetHealthRequest& /*request*/, adpp::Response& response,
                       const ProviderRuntime& runtime) {
    const ReadinessReport report = runtime.readiness();
    auto* out = response.mutable_get_health();
    *out->mutable_provider() = make_provider_health(report);

    // Health for the live inventory, plus any startup-failed devices that are no
    // longer live (so get_health still surfaces a device that failed to init).
    std::vector<std::string> ids = runtime.list_device_ids();
    std::unordered_set<std::string> live(ids.begin(), ids.end());
    for (const auto& failure : report.failed_devices) {
        if (live.find(failure.device_id) == live.end()) {
            ids.push_back(failure.device_id);
        }
    }
    for (auto& health : make_device_health(ids, report)) {
        *out->add_devices() = std::move(health);
    }
    set_status_ok(response);
}

void handle_unimplemented(adpp::Response& response, const std::string& message) {
    set_status(response, adpp::Status::CODE_UNIMPLEMENTED, message);
}

}  // namespace anolis::provider_sdk::handlers
