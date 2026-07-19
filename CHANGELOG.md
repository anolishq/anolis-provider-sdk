# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `docs/metrics.md`: reserved device-health metric vocabulary
  (`io_ok` / `io_failed` / `io_retried_attempts`, protocol-counter
  separation, `last_seen` honesty) so independent providers stay
  consistent without sharing code. Contract-only — the SDK defines key
  semantics; each provider implements them at the layer that performs
  the attempts. Both bread (0.3.3+) and ezo (0.3.1+) already conform
  (anolishq/anolis-provider-ezo#100).

## [0.1.3] — 2026-07-04

Maintenance release (docs + CI only; no library code changes).

### Changed

- CI: bump the shared `setup-vcpkg` pin to v2.3. (#13)

### Documentation

- README/CHANGELOG reflect the shipped v0.1.2 surface. (#15)
- Replace private repo references with the public install path. (#14)

## [0.1.2] — 2026-06-26

### Fixed

- `apply_min_timestamp` no longer masks `FAULT`/`UNKNOWN` qualities as `STALE`
  (staleness downgrades only an `OK` reading). (#12)

## [0.1.1] — 2026-06-26

### Added

- Per-device health enrichment hook: adapters can supply `DeviceHealth`
  metrics + `last_seen` for the provider health report. (#11)

## [0.1.0] — 2026-06-26

First consumable release (epic anolis-protocol#45). Note: this entry was
originally written at scaffold time and under-described the tag — v0.1.0
already shipped the lifted spine and device-model framework (D.3b/D.3c),
not a placeholder API.

### Added

- **The spine**: `ProviderRuntime` + `run_loop` (§3.2 Hello gate, dispatch,
  exit codes), generic ADPP handlers, framed-stdio transport, config toolkit,
  logging and signal helpers.
- **The device-model framework**: header-only `DeviceAdapter<HandleT>`,
  neutral `AdapterReadResult`/`AdapterCallResult` types, quality/staleness
  helpers, `device_spec`.
- CMake build that FetchContent's `anolis-protocol` v1.6.0 and re-exports the
  ADPP proto as the `anolis::adpp_proto` target (the SDK now owns the proto pin +
  protobuf/vcpkg baseline for the fleet).
- The `anolis::provider_sdk` static library target (C++20) carrying the public
  header tree under `src/anolis/provider_sdk/`.
- CI (`ok` aggregator: Linux/Windows build+test, clang-format, diff-only
  clang-tidy gate, version-sync), TSAN hardening lane, weekly dependency CVE
  scan, and a source-tarball release workflow.
- Org hygiene: pinned clang-tools (18.1.8), shared `Warnings.cmake`, vcpkg
  triplets, `tsan.supp` / `valgrind.supp`, Renovate, `AGENTS.md`.
- A proto smoke test proving the re-export links and round-trips a message.

[Unreleased]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.3...HEAD
[0.1.3]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/anolishq/anolis-provider-sdk/releases/tag/v0.1.0
