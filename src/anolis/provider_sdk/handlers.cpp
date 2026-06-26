#include "anolis/provider_sdk/handlers.hpp"

#include <string>

#include "anolis/provider_sdk/transport.hpp"  // kMaxFrameBytes for the Hello metadata

namespace anolis::provider_sdk::handlers {
namespace {

void set_status(adpp::Response& response, adpp::Status::Code code, const std::string& message) {
    response.mutable_status()->set_code(code);
    response.mutable_status()->set_message(message);
}

void set_status_ok(adpp::Response& response) { set_status(response, adpp::Status::CODE_OK, "ok"); }

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

void handle_unimplemented(adpp::Response& response, const std::string& message) {
    set_status(response, adpp::Status::CODE_UNIMPLEMENTED, message);
}

}  // namespace anolis::provider_sdk::handlers
