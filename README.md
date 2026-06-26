# anolis-provider-sdk

Shared C++ SDK for [Anolis Device Provider Protocol](https://github.com/anolishq/anolis-protocol)
(ADPP) device providers. It holds the parts that the three reference providers —
[`anolis-provider-ezo`](https://github.com/anolishq/anolis-provider-ezo),
[`anolis-provider-sim`](https://github.com/anolishq/anolis-provider-sim), and
[`anolis-provider-bread`](https://github.com/anolishq/anolis-provider-bread) —
share, so a new provider is "implement the device seam, get the protocol for free."

> **Status: scaffold (0.1.0).** The repository is being stood up as the home for
> the Wave-5 extraction (epic [anolis-protocol#45](https://github.com/anolishq/anolis-protocol/issues/45)).
> The spine and device-model framework are lifted in subsequent steps; today the
> repo builds, re-exports the ADPP proto, and passes a smoke test.

## What it provides (target shape)

- **The spine** — framed-stdio transport (`uint32_le` length-prefixed protobuf),
  the request dispatch / run-loop, the lifecycle/handshake handler skeleton, the
  config-validation toolkit, and structured logging.
- **The device-model framework** — the header-only `DeviceAdapter<HandleT>`
  descriptor (templated over the provider's hardware-session handle), the neutral
  `AdapterReadResult` / `AdapterCallResult` result types, the `quality_from`
  staleness helper, and `default_ids_from` for the §7.2 default signal set.
- **The proto re-export** — the SDK is the single declarant of the
  `anolis-protocol` pin and the protobuf/vcpkg baseline for the fleet, exposed as
  the `anolis::adpp_proto` target.

Provider-specific concerns (sim physics/actuation, bread CRUMBS framing, ezo's
i2c bus executor, each provider's device taxonomy) stay in the provider.

## Consuming it

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

## Building locally

```sh
cmake --preset ci-linux-release
cmake --build --preset ci-linux-release --parallel
ctest --preset ci-linux-release
```

`just check && just test` wraps formatting, lint, build, and tests. vcpkg
resolves dependencies during CMake configure (`VCPKG_ROOT` must be set).

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
