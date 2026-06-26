# AGENTS.md — anolis-provider-sdk

> Per-repo conventions for coding agents (Claude Code, OpenCode, …). The
> canonical cross-repo rules — Conventional Commits, minimal-first/YAGNI, no
> secrets, run checks before asserting success — live in the user's **global**
> `AGENTS.md` and are not repeated here. This file records only what is
> **specific to this repo**. Backlog lives in GitHub issues, **not** a TODO.md.

## What this repo is

The shared C++ SDK extracted from the three ADPP device providers
(`anolis-provider-{ezo,sim,bread}`) in Wave 5 (epic anolis-protocol#45). It will
hold the ~96%-identical **spine** (framed-stdio transport, run-loop, handler
skeleton, config toolkit, logging) plus the **device-model framework** (the
header-only `DeviceAdapter<HandleT>` descriptor, neutral `AdapterReadResult`/
`AdapterCallResult` result types, `quality_from`, `default_ids_from`). Providers
consume it as **source via FetchContent** — never a prebuilt binary.

## Build / test

- C++20 (`std::format` for diagnostics). Build and test via presets:
  `cmake --preset ci-linux-release` → `cmake --build --preset ci-linux-release`
  → `ctest --preset ci-linux-release`. `just check && just test` wraps this.
- The required CI status check is the **`ok`** job (it aggregates the lanes);
  never bypass it, and never merge red.

## Tooling

- clang-format / clang-tidy are pinned to **18.1.8** via the shared
  `setup-clang-tools` action — do NOT use pip/apt/pre-commit/container versions.
  Run `clang-format -i` before **every** commit (CI fails otherwise).
  `tests/.clang-tidy` relaxes test-only check noise; production checks still apply.
- Shared `.github` actions/workflows are SHA-pinned with a `# <tag>` comment so
  Renovate can track them — keep that comment when bumping. vcpkg comes from the
  shared `setup-vcpkg` action.

## Repo-specific gotchas

- **The SDK is the single declarant of the `anolis-protocol` proto pin + the
  protobuf/vcpkg baseline for the fleet.** The proto is re-exported as the
  `adpp_proto` OBJECT target (alias `anolis::adpp_proto`) + the `protocol.pb.h`
  shim. When a provider migrates onto the SDK it **deletes its own proto block**
  in the same commit it links `anolis::provider_sdk` — two `add_library(adpp_proto …)`
  in one FetchContent tree, or divergent pins, is a hard error / silent skew.
- **Consumed as source/FetchContent, never prebuilt.** TSAN / `x64-linux-static`
  / `arm64` all rebuild protobuf in-tree with the consumer's flags. Do not add an
  `install()`/`export()`/`find_package` config until a packaged consumer exists.
- **Public headers live under `include/anolis/provider_sdk/`** (the library API
  surface), not `src/` — `.clang-tidy`'s `HeaderFilterRegex` includes `include/`
  for that reason. `src/` is private implementation.
- **The device descriptor is `DeviceAdapter<HandleT>` — templated over the
  provider's session handle** (ezo `EzoHandle`, bread `crumbs::Session`, sim
  `std::monostate`). Handle acquisition is provider-local and happens before
  `read_signals`/`call`. The taxonomy (each provider's `enum` + `-Werror=switch`
  `adapter_for`) stays provider-local; the SDK never names a provider's device types.
