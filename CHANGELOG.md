# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] — scaffold

Initial repository scaffold (epic anolis-protocol#45, step #52). Establishes the
build, CI, and hygiene foundation so the spine and device-model framework have a
home; no provider-facing API yet.

### Added

- CMake build that FetchContent's `anolis-protocol` v1.6.0 and re-exports the
  ADPP proto as the `anolis::adpp_proto` target (the SDK now owns the proto pin +
  protobuf/vcpkg baseline for the fleet).
- The `anolis::provider_sdk` static library target (C++20) carrying the public
  header tree under `include/anolis/provider_sdk/` (placeholder API surface).
- CI (`ok` aggregator: Linux/Windows build+test, clang-format, diff-only
  clang-tidy gate, version-sync), TSAN hardening lane, weekly dependency CVE
  scan, and a source-tarball release workflow.
- Org hygiene: pinned clang-tools (18.1.8), shared `Warnings.cmake`, vcpkg
  triplets, `tsan.supp` / `valgrind.supp`, Renovate, `AGENTS.md`.
- A proto smoke test proving the re-export links and round-trips a message.

[Unreleased]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/anolishq/anolis-provider-sdk/releases/tag/v0.1.0
