# Contributing to anolis-provider-sdk

This is the shared SDK consumed by the ADPP providers, so a change here can move
all three at once. Keep the bar high and the surface small.

## Ground rules

- **Conventional Commits** (`type(scope): description`), imperative, ≤ 72 chars.
- **Minimal-first / YAGNI.** The SDK lifts what is genuinely shared across
  providers; provider-specific behavior stays in the provider. Don't add an axis
  to the public API to serve one provider — extend it provider-local.
- **Never merge red.** The required check is the aggregated **`ok`** job.
- Run `clang-format -i` before every commit; `just check && just test` mirrors CI.

## Build / test

```sh
cmake --preset ci-linux-release
cmake --build --preset ci-linux-release --parallel
ctest --preset ci-linux-release
```

vcpkg resolves dependencies during configure (`VCPKG_ROOT` must point at a vcpkg
checkout). clang-format / clang-tidy are pinned to 18.1.8 via the shared
`setup-clang-tools` action — match that locally (workstation-configs ships it).

## Changing the public API

The public surface is the header tree under `include/anolis/provider_sdk/`. Any
change to the `DeviceAdapter<HandleT>` descriptor, the neutral result types, or
the spine handler contract is a change to **all** providers' compile + behavior.

- The acceptance net is per-provider **ADPP conformance** staying green
  commit-to-commit through each migration, plus the 0F cross-version matrix.
- The SDK stays `0.x` until all three providers ship on it; expect seams to move.
- Don't promote a provider's local shape (e.g. a flat numeric address) into the
  neutral SDK shape without confirming it serves all three — see the Wave-5
  design decisions on the epic.

## Updating the proto pin

The SDK is the **single declarant** of the `anolis-protocol` FetchContent pin.
Bump it here (CMake `URL` + `URL_HASH`), not in the providers — providers stop
pinning the proto once they consume the SDK.
