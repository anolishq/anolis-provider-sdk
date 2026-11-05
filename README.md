# anolis-provider-sdk

Shared C++ SDK for [Anolis Device Provider Protocol](https://github.com/anolishq/anolis-protocol)
(ADPP) device providers. It holds the parts that the three reference providers —
[`anolis-provider-ezo`](https://github.com/anolishq/anolis-provider-ezo),
[`anolis-provider-sim`](https://github.com/anolishq/anolis-provider-sim), and
[`anolis-provider-bread`](https://github.com/anolishq/anolis-provider-bread) —
share, so a new provider is "implement the device seam, get the protocol for free."

> **Status: v0.6.0 — the spine and device-model framework are lifted and
> consumed by all three reference providers** (each pins its own SDK release
> tarball; the SDK is the fleet's single declarant of the `anolis-protocol`
> build pin, v1.10.0; each provider's CI pins its own `anolis-protocol[conformance]`
> wheel). Extraction history: epic
> [anolis-protocol#45](https://github.com/anolishq/anolis-protocol/issues/45).

## What it provides

- **The spine** — framed-stdio transport (`uint32_le` length-prefixed protobuf),
  the request dispatch / run-loop, the lifecycle/handshake handler skeleton, the
  config-validation toolkit, and structured logging.
- **The device-model framework** — the header-only `DeviceAdapter<HandleT>`
  descriptor (templated over the provider's hardware-session handle), the neutral
  `AdapterReadResult` / `AdapterCallResult` result types, the `quality_from`
  staleness helper, and `default_ids_from` for the §7.2 default signal set.
- **The proto re-export** — the SDK is the single declarant of the
  `anolis-protocol` build pin, exposed as the `anolis::adpp_proto` target. Each
  provider declares its own protobuf dependency and vcpkg baseline.
- **Host requirements and claims** (anolishq/anolis#318) — the `--check-host`
  envelope and its readiness keys (`host_check.hpp`, executable profile v1 §6),
  the `anolis.claim` device tag (`claims.hpp`), and, in the I2C module, the
  canonical I2C claim key (`i2c/claims.hpp`) and reusable i2c-dev host checks
  (`i2c/host_checks.hpp`).

- **Contracts, not code, where code would couple** — the reserved device-health
  metric vocabulary lives in [`docs/metrics.md`](docs/metrics.md): key names and
  semantics the SDK pins, implemented per-provider at whatever layer is honest
  for its transport.

Provider-specific concerns (sim physics/actuation, bread CRUMBS framing, ezo's
i2c bus executor, each provider's device taxonomy) stay in the provider.

## Consuming it

To start a provider, use the template:
[anolis-provider-template-cpp](https://github.com/anolishq/anolis-provider-template-cpp)
is a working provider on this SDK (simulated heaters) with the build below, CI
on x86_64 and arm64, a release workflow, the conformance harness and a rename
script. Click **Use this template**, then run `scripts/rename.sh <kind>`.

The SDK is consumed **as source**, via CMake `FetchContent` — never as a prebuilt
binary, so sanitizer / static / arm64 builds rebuild the dependency tree in-tree
with the consumer's flags:

```cmake
include(FetchContent)
FetchContent_Declare(
  anolis_provider_sdk
  URL https://github.com/anolishq/anolis-provider-sdk/releases/download/vX.Y.Z/anolis-provider-sdk-X.Y.Z-source.tar.gz
  URL_HASH SHA256=...
)
FetchContent_MakeAvailable(anolis_provider_sdk)
target_link_libraries(my_provider PRIVATE anolis::provider_sdk)
```

The provider supplies Protobuf, Threads and, unless
`ANOLIS_PROVIDER_SDK_ENABLE_CONFIG` is off, yaml-cpp, through its own vcpkg
manifest and baseline. See [docs/index.md](docs/index.md#using-it).

## Building locally

```sh
cmake --preset ci-linux-release
cmake --build --preset ci-linux-release --parallel
ctest --preset ci-linux-release
```

`just check && just test` wraps formatting, lint, build, and tests. vcpkg
resolves dependencies during CMake configure (`VCPKG_ROOT` must be set).

## License

Apache-2.0. See [LICENSE](LICENSE). A provider that links this SDK can be
distributed under any licence compatible with Apache-2.0, proprietary included.

Releases up to and including v0.4.0 were published under AGPL-3.0-or-later and
remain available under it. The Anolis runtime and Workbench stay AGPL-3.0; a
provider is a separate process, so their licence does not extend to it.
