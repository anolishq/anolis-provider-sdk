# anolis-provider-sdk

A C++ library for writing Anolis device providers. It implements the provider
side of the Anolis Device Provider Protocol (ADPP), so a provider supplies only
its devices. anolis-provider-sim, anolis-provider-bread and
anolis-provider-ezo are built on it.

## What it provides

- **Protocol plumbing:** the framed standard-input/output transport, the
  request loop, the handshake and lifecycle handlers, and logging.
- **Device model:** the `DeviceAdapter<HandleT>` descriptor a provider fills in
  for each device type, the read and call result types, and helpers for
  signal quality and the default signal set.
- **Configuration:** declare a provider's config once and get the
  `--config-schema` output, `--check-config` validation and typed parsing from
  that declaration. See [Config-schema toolkit](./config-schema.md).
- **Host checks and bus claims:** the `--check-host` report, and the
  `anolis.claim` tag a provider uses to declare the I2C addresses it owns.
- **Health metrics:** reserved metric names and their meaning, in
  [Device health metrics](./metrics.md).

Device-specific behaviour, such as simulated physics or a bus protocol, stays
in each provider.

## Using it

Fetch a release's source with CMake `FetchContent` and link the library:

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

The SDK is built from source as part of the provider, never installed as a
prebuilt library. It also provides the ADPP protocol messages as the
`anolis::adpp_proto` target.

The provider's build supplies the SDK's dependencies: `find_package` must find
Protobuf and Threads, and yaml-cpp too unless the config toolkit is turned off
with `-DANOLIS_PROVIDER_SDK_ENABLE_CONFIG=OFF`. With vcpkg, list them in the
provider's own manifest under its own baseline; the SDK does not bring one.
