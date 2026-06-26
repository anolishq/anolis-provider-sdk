#include "anolis/provider_sdk/runtime.hpp"

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "anolis/provider_sdk/handlers.hpp"
#include "anolis/provider_sdk/transport.hpp"
#include "protocol.pb.h"

namespace anolis::provider_sdk {
namespace {

// Fires the provider's shutdown hook on every exit from run_loop (clean EOF and
// the parse/serialize/write-failure returns alike) — mirrors sim's
// PhysicsShutdownGuard.
class ShutdownGuard {
public:
    explicit ShutdownGuard(const LifecycleHooks& hooks) : hooks_(hooks) {}
    ~ShutdownGuard() { hooks_.on_shutdown(); }

private:
    const LifecycleHooks& hooks_;
};

}  // namespace

int run_loop(std::istream& input, std::ostream& output, ProviderRuntime& runtime, const LifecycleHooks& hooks) {
    const ShutdownGuard shutdown_guard(hooks);

    std::vector<uint8_t> frame;
    std::string io_err;
    // ADPP L2 §3.2: a non-Hello request before a successful Hello is rejected
    // with CODE_FAILED_PRECONDITION. The session is this process's stdio stream,
    // so a local flag suffices.
    bool hello_completed = false;

    while (true) {
        frame.clear();
        if (!transport::read_frame(input, frame, io_err)) {
            return io_err.empty() ? 0 : 2;  // clean EOF vs read error
        }

        adpp::Request request;
        if (!request.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
            return 3;
        }

        adpp::Response response;
        response.set_request_id(request.request_id());
        response.mutable_status()->set_code(adpp::Status::CODE_INTERNAL);
        response.mutable_status()->set_message("uninitialized");

        if (request.has_hello()) {
            handlers::handle_hello(request.hello(), response, runtime);
            if (response.status().code() == adpp::Status::CODE_OK) {
                hello_completed = true;
            }
        } else if (!hello_completed) {
            response.mutable_status()->set_code(adpp::Status::CODE_FAILED_PRECONDITION);
            response.mutable_status()->set_message("Hello handshake required before any other request");
        } else if (request.has_wait_ready()) {
            handlers::handle_wait_ready(request.wait_ready(), response, runtime);
            hooks.on_wait_ready();
        } else if (request.has_list_devices()) {
            handlers::handle_list_devices(request.list_devices(), response, runtime);
        } else if (request.has_describe_device()) {
            handlers::handle_describe_device(request.describe_device(), response, runtime);
        } else if (request.has_read_signals()) {
            handlers::handle_read_signals(request.read_signals(), response, runtime);
        } else if (request.has_call()) {
            handlers::handle_call(request.call(), response, runtime);
        } else if (request.has_get_health()) {
            handlers::handle_get_health(request.get_health(), response, runtime);
        } else {
            handlers::handle_unimplemented(response);
        }

        std::string response_bytes;
        if (!response.SerializeToString(&response_bytes)) {
            return 4;
        }
        if (!transport::write_frame(output, reinterpret_cast<const uint8_t*>(response_bytes.data()),
                                    response_bytes.size(), io_err)) {
            return 5;
        }
    }
}

}  // namespace anolis::provider_sdk
