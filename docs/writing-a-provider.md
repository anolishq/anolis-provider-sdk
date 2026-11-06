# Writing a provider

A provider is an executable the runtime starts as a child process. It reads
ADPP requests from standard input and writes responses to standard output; the
SDK handles that loop. A provider built on the SDK supplies three things: an
implementation of `ProviderRuntime`, a `main()` that hands control to
`run_loop`, and the command-line flags the executable profile requires.

The quickest start is the
[template](https://github.com/anolishq/anolis-provider-template-cpp): a working
provider with its build, CI, release and the conformance harness already set
up. Its `src/provider.cpp`, the same as this repository's reference provider
(`examples/reference/`), is the smallest complete example to read alongside
this page: it implements every flag below.

## 1. Implement `ProviderRuntime`

`ProviderRuntime` (`anolis/provider_sdk/runtime.hpp`) is the interface the SDK
calls for each request:

| Member | Answers |
| --- | --- |
| `metadata()` | Provider name, version and protocol version, sent in the Hello handshake |
| `readiness()` | Which configured devices started and which failed, for WaitReady and health |
| `list_device_ids()`, `has_device()`, `device_info()` | The device inventory |
| `capabilities(device_id)` | A device's signals and functions |
| `read(device_id, signal_ids)` | Current signal values |
| `call(device_id, function_id, args)` | Run a function |
| `resolve_function_id(device_id, name)` | Map a function name to its id |

`device_health()` and `provider_health()` are optional, with defaults that add
nothing. They must not do device I/O. Status that needs a device read, such as
a firmware watchdog's, is read where the provider already does I/O and cached,
and health reports the cached reading, with its age where the
[metrics vocabulary](./metrics.md#command-watchdog-state) defines one.

`call` sees only arguments that match the function's declared `ArgSpec`s: the
SDK checks them first (semantics §8.3) and answers `CODE_INVALID_ARGUMENT` or
`CODE_OUT_OF_RANGE` itself. So declare each argument's type, whether it is
required, and its bounds or `allowed_values`, and check in `call` only what an
`ArgSpec` cannot express.

A function that declares results (`FunctionSpec.results`) returns them with
`call_ok(results)`, and the response carries exactly those. A call that returns
no results gets the constant `accepted = true`.

The SDK handles a call's deadline (`CallRequest.deadline`, semantics §8.4) and
advertises `supports_deadlines = "true"` in Hello. The deadline is an absolute
UTC time, compared with the wall clock. After the SDK's own checks (device,
function name, and the arguments of a declared function), a deadline that is
not a valid timestamp gets `CODE_INVALID_ARGUMENT`, and one already reached
gets `CODE_DEADLINE_EXCEEDED`; in both cases `call` does not run. A
`function_id` the device does not declare is left to `call` to reject, so for
one of those the deadline is reported first. The SDK makes no attempt to
cancel a running `call` and does not pass it the deadline, so `call` cannot
attempt one either: the best-effort cancel §8.4 asks for is not provided. If the deadline
passes while `call` runs, the call completes, and the response is
`CODE_DEADLINE_EXCEEDED` with `side_effects = possible` in the status details,
without the call's results, because its effects may have happened. An error
`call` returns is sent as it is, not replaced by `CODE_DEADLINE_EXCEEDED`. So keep `call` short and do not start long
waits in it. The Python SDK behaves the same way, except that it answers an
unknown function with `CODE_NOT_FOUND` before looking at the deadline. The SDK
enforces deadlines even if a provider sets `supports_deadlines` to `"false"`
through `hello_extra`.

Inside `read` and `call`, each provider dispatches to a per-device-type
adapter of its own. The SDK also offers an optional template for that table,
`DeviceAdapter<HandleT>` in `device_adapter.hpp`, parameterised on the
provider's hardware session type.

### Advertise WaitReady

The SDK's Hello response carries `transport`, `max_frame_bytes` and
`supports_deadlines`. The
[framed-stdio profile](https://github.com/anolishq/anolis-protocol/blob/main/docs/profiles/framed-stdio-v1.md)
also requires `supports_wait_ready`, so add it through
`ProviderMetadata::hello_extra`. The runtime sends WaitReady only when it is
`"true"`. A provider that advertises it reports `init_time_ms` in the WaitReady
diagnostics through `ReadinessReport::extra_diagnostics`; the conformance
harness checks for it.

```cpp
namespace sdk = anolis::provider_sdk;

sdk::ProviderMetadata MyProviderRuntime::metadata() const {
    sdk::ProviderMetadata meta;
    meta.name = "my-provider";
    meta.version = "0.1.0";
    meta.hello_extra["supports_wait_ready"] = "true";
    return meta;
}

sdk::ReadinessReport MyProviderRuntime::readiness() const {
    sdk::ReadinessReport report;
    // ... configured, started and failed devices ...
    report.extra_diagnostics["init_time_ms"] = std::to_string(init_time_ms_);
    return report;
}
```

### Set a poll rate on polled signals

The runtime polls only the signals whose `SignalSpec.poll_hint_hz` is greater
than zero, and serves device state only from what it polled. Set a positive
`poll_hint_hz` in `capabilities()` on every signal the runtime should show; a
signal left at zero never appears in the runtime's state.

`SignalDefinition::is_default` is separate: it selects the signals a
ReadSignals request with empty `signal_ids` returns, and the runtime does not
use it.

## 2. Hand control to `run_loop`

```cpp
#include <iostream>
#include "anolis/provider_sdk/runtime.hpp"

int main(int argc, char** argv) {
    // parse flags (step 3), load and validate the config, build the runtime
    MyProviderRuntime runtime(config);
    return anolis::provider_sdk::run_loop(std::cin, std::cout, runtime);
}
```

`run_loop` returns the process exit code: 0 when standard input closes, 2 for
a framing or read error, 3 for a request that does not parse (the codes of the
framed-stdio profile, §4), and 5 when a response cannot be written to standard
output. A response too large for a frame (1 MiB) is not an exit: the SDK
answers that request with `CODE_RESOURCE_EXHAUSTED` instead and keeps serving,
and request fields quoted in its own error messages are cut to 64 bytes. An
optional
`LifecycleHooks` argument runs code after the handshake (`initialize`), after
WaitReady and on shutdown.

Write nothing but responses to standard output; logs go to standard error. On
Windows, put standard input and output in binary mode before the loop.

## 3. Command-line flags

The [executable profile](https://github.com/anolishq/anolis-protocol/blob/main/docs/profiles/anolis-executable-profile-v1.md)
defines the flags tools rely on:

| Flag | Does |
| --- | --- |
| `--config <path>` | Start serving with this config |
| `--check-config <path>` | Validate the config and exit |
| `--config-schema` | Print the config's JSON Schema envelope and exit |
| `--check-host <path>` | Report whether this host meets the config's requirements, without actuating anything |
| `--version` | Print the version and exit |

The SDK's [config-schema toolkit](./config-schema.md) produces the
`--config-schema`, `--check-config` and parsing behaviour from one declaration,
and `host_check.hpp` produces the `--check-host` report.

## 4. Build

Add the SDK with `FetchContent` and link `anolis::provider_sdk`, as shown on
the [SDK index](./index.md#using-it). The ADPP messages come with it as
`anolis::adpp_proto`; do not declare the protocol separately.

## 5. Run it under the runtime

The runtime starts the provider with exactly the `args` in its config and adds
nothing, so pass `--config` there:

```yaml
providers:
  - id: my0
    command: path/to/my-provider
    args: ["--config", "path/to/my-provider.yaml"]
```

## 6. Check conformance

The `anolis-adpp-conformance` harness from anolis-protocol drives the built
executable through the protocol and the executable profile. Each provider keeps
a mock-mode config and a `--provider-profile` manifest for it; see
[ADPP-CONFORMANCE.md](https://github.com/anolishq/anolis-protocol/blob/main/conformance/ADPP-CONFORMANCE.md).
