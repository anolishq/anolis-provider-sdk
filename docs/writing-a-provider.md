# Writing a provider

A provider is an executable the runtime starts as a child process. It reads
ADPP requests from standard input and writes responses to standard output; the
SDK handles that loop. A provider built on the SDK supplies three things: an
implementation of `ProviderRuntime`, a `main()` that hands control to
`run_loop`, and the command-line flags the executable profile requires.

anolis-provider-ezo is the smallest complete example to read alongside this
page: it implements every flag below on a current SDK release.

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
nothing. They must not do device I/O.

Inside `read` and `call`, each provider dispatches to a per-device-type
adapter of its own. The SDK also offers an optional template for that table,
`DeviceAdapter<HandleT>` in `device_adapter.hpp`, parameterised on the
provider's hardware session type.

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

`run_loop` returns the process exit code: 0 when standard input closes, and
2 to 5 for read, parse, serialize and write errors. An optional
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

## 5. Check conformance

The `anolis-adpp-conformance` harness from anolis-protocol drives the built
executable through the protocol and the executable profile. Each provider keeps
a mock-mode config and a `--provider-profile` manifest for it; see
[ADPP-CONFORMANCE.md](https://github.com/anolishq/anolis-protocol/blob/main/conformance/ADPP-CONFORMANCE.md).
