# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Call deadlines** (semantics §8.4). Hello advertises
  `supports_deadlines = "true"`, and `handle_call` checks
  `CallRequest.deadline` (an absolute UTC time, compared with the wall clock)
  after the SDK's own checks (device, function name, and the §8.3 arguments of
  a declared function): a deadline outside the valid
  `google.protobuf.Timestamp` range is `CODE_INVALID_ARGUMENT`, and one already
  reached is `CODE_DEADLINE_EXCEEDED`, in both cases before
  `ProviderRuntime::call` runs. A `function_id` the device does not declare is
  still the provider's to reject, so for one of those the deadline is reported
  first, where the Python SDK answers `CODE_NOT_FOUND`. The SDK does not
  cancel a running call, and `ProviderRuntime::call` is not given the
  deadline, so a provider cannot attempt a cancel either; §8.4 asks for a
  documented best-effort cancel, and this release makes none (nor does the
  Python SDK). If the deadline passes while a call runs, the response is
  `CODE_DEADLINE_EXCEEDED` with `side_effects = possible` in the status
  details and no results. An error the call returns is sent unchanged rather
  than as `CODE_DEADLINE_EXCEEDED`, as in the Python SDK; §8.4 does not say
  which wins. Every
  provider on this release advertises deadline support with no code change; a
  call without a deadline behaves as before. The reference provider's
  conformance lane now runs the two §8.4 tests and allows no skips. (#71)

### Changed

- **Watchdog health metrics come from a cached reading.** `docs/metrics.md`
  required the `watchdog_*` keys to come from a live status query, which the
  in-process-only contract of `ProviderRuntime::device_health` (`runtime.hpp`)
  forbids, so no provider could meet both. The contract now keeps the hook
  in-process: the provider reads the watchdog status on its own I/O path,
  caches it, and `device_health` reports the cached reading. A new reserved
  key, `watchdog_status_age_ms`, gives that reading's age and is present
  whenever the other watchdog keys are, measured on a monotonic clock. When
  a later status read fails, the provider keeps reporting the last successful
  reading and its age keeps growing; before, a failed query left the keys
  absent. After `clear_watchdog_trip` the provider reads the status back
  before the call returns, so the next health read reflects the clear; if that
  read fails or still shows the trip, so does the call. No SDK code or API
  changes, but the meaning of the watchdog keys on the wire changes: consumers
  that read `watchdog_armed` or `watchdog_tripped` as current must now check
  `watchdog_status_age_ms`. Consumer follow-ups: anolishq/anolis#732,
  anolishq/anolis-workbench#809. (#62, anolishq/anolis-provider-bread#199)
- **`run_loop` no longer returns exit code 4.** It documented 4 for a response
  that failed to serialize; that response is now answered with
  `CODE_INTERNAL` and the loop goes on, so 4 has no remaining cause. A
  provider that maps 4 to something of its own will not see it. 5 now means
  only a failed write to standard output. (#50)

### Fixed

- **A response over the 1 MiB frame cap no longer ends the provider.**
  `run_loop` wrote nothing and returned 5 when a response would not fit in a
  frame, so one well-formed request under the cap could kill a provider: a
  `DescribeDevice` for an unknown `device_id` of about 1 MiB, whose
  `NOT_FOUND` message quoted the id in full, or a handler result that was
  simply large. Such a response is now replaced by `CODE_RESOURCE_EXHAUSTED`
  for the same `request_id` (`CODE_INTERNAL` if it fails to serialize), as
  the Python SDK does, and the loop keeps serving. The SDK's own error
  messages quote at most 64 bytes of a request's `device_id`, `signal_id`,
  `function_name` or undeclared argument name, cut on a UTF-8 character
  boundary and marked `...`. A response of exactly 1 MiB is still sent as it
  is. `runtime.hpp` documents the exit codes against the framed-stdio
  profile: 0/2/3 as it defines them, plus 5 for a failed write to standard
  output, which the profile does not list. (#50)

## [0.6.0] — 2026-11-04

### Added

- **Call arguments are validated against the declared spec** (semantics §8.3)
  before `ProviderRuntime::call` runs, in the Python SDK's order: an undeclared
  argument, a missing required one, a wrong type, a non-finite double or a value
  outside `allowed_values` is `CODE_INVALID_ARGUMENT`; a value outside its
  declared (inclusive) bounds is `CODE_OUT_OF_RANGE`. A provider's own checks
  that the declared `ArgSpec` already expresses can go. A function the device
  does not declare in `capabilities()` is passed to the provider as before.
  `validate_call_args` (`call_validation.hpp`) is public for providers that
  validate elsewhere. (#65)
- **Per-function call results.** `AdapterCallResult` gains `results`, the
  function's declared results; `call_ok(results)` builds one. When a call
  returns results, the response carries exactly those; when it returns none,
  the constant `accepted = true` as before. `results` defaults to empty, and a
  three-element `{ok, code, message}` initializer still compiles warning-free,
  so existing providers need no change. (#60)
- A call to a device the provider does not list is `CODE_NOT_FOUND` from the
  SDK, as describe and read already were, before `ProviderRuntime::call` runs.
- **A reference provider, `anolis-provider-reference`** (`examples/reference/`):
  simulated heaters, the same device as the Python SDK's reference provider and
  the provider templates. CI runs the anolis-protocol 1.10.0 conformance harness
  against it on every change; the only skips allowed are the two §8.4 deadline
  tests, until the SDK supports deadlines (#71). It is built only when the SDK
  is the top-level project, never for a consumer. (#64)

## [0.5.0] — 2026-11-03

0.5.0, not 0.4.1: the licence changes, though no API does.

### Changed

- **Licence: Apache-2.0**, from AGPL-3.0-or-later. Providers link this SDK
  statically, so its licence applied to every provider binary built with it;
  a provider can now be distributed under any Apache-2.0-compatible licence,
  proprietary included. The runtime and Workbench stay AGPL-3.0. Releases up to
  and including v0.4.0 remain AGPL-3.0-or-later. (#63)
- anolis-protocol build pin 1.6.0 -> 1.10.0, the first release under
  Apache-2.0, so the schema the SDK compiles carries the same licence as the
  SDK. Between 1.6.0 and 1.10.0 the `.proto` files changed only in a comment,
  so the generated code is unchanged.

## [0.4.0] — 2026-10-22

### Added

- `LifecycleHooks::initialize(budget)` runs once, after a successful Hello
  and before the first request after it is handled
  (anolishq/anolis-protocol#76). A provider whose start-up is slow, such as
  bread probing an unpowered bus, can move it there and answer Hello at
  once. Normally the first request is WaitReady, and `budget` is its
  `max_wait_ms_hint`; for any other first request it is 0, meaning no hint.
  The hook runs on the loop's thread and blocks that reply, so the WaitReady
  reply reflects what it did. The default does nothing, so providers that
  don't set it behave as before.

### Fixed

- The `protocol.pb.h` compatibility shim is written with `file(CONFIGURE)`,
  which leaves it untouched when its content is unchanged. It was rewritten
  on every configure, so a reconfigure that changed nothing rebuilt every
  translation unit that includes it. (#41)

### Changed

- CI, the hardening lane and the release workflow's build-and-test run on
  ubuntu-26.04. (#44) The release publishes source only, and providers build
  it with their own toolchain, so nothing a provider ships changes.

## [0.3.1] — 2026-10-16

Maintenance release (docs + CI only; no library code changes).

### Changed

- CI: bump the shared `anolishq/.github` actions to v2.4 and refresh the
  pinned action digests. (#36, #40)

### Documentation

- `docs/metrics.md`: reserve `firmware_version` as a device identity string
  — the firmware revision the device itself reports, as an opaque string;
  omitted when the device reports none, never parsed as a version, and
  distinct from ADPP's `type_version`. (#38, #39)
- Correct the 0.3.0 entry: a provider's `timeout_ms` config keeps its other
  uses; only the `LinuxI2cBus` constructor parameter that fed `I2C_TIMEOUT`
  went away. (#35)

## [0.3.0] — 2026-10-08

### Added

- **Opaque claims** (`claims.hpp`, anolishq/anolis#318): `kClaimTag`
  (`anolis.claim`) and `add_claim()`, which adds a key to a device's
  space-separated claim tag without repeats. The runtime is moving its
  ownership check from parsing `hw.bus_path` / `hw.i2c_address` to exact-string
  uniqueness over these keys.
- **I2C claim key** (`i2c::claim_key`): `i2c:<bus_path>:0x<aa>`, lowercase
  two-digit hex — the form the runtime normalized the old tags to, so moving to
  claims changes no collision.
- **`--check-host` support** (`host_check.hpp`, executable profile v1 §6): the
  `Requirement` type, `write_envelope()` (prints the envelope and returns the
  exit code, 1 exactly when something is unmet), and `readiness_diagnostics()`
  for the `host_check` / `host_unmet` readiness keys. Host text that is not
  valid UTF-8 is replaced, so the envelope always parses.
- **i2c-dev host checks** (`i2c::check_host`), read-only: `i2c.bus_present`,
  `i2c.bus_access` (opens the node read-write; on denial names the node's group
  and mode; `unknown` when run as root, which opens anything), and, given a
  maximum, `i2c.bus_clock` from the device tree's `clock-frequency` (`unknown`
  where the platform does not expose it).

### Changed

- **`LinuxI2cBus` no longer sets `I2C_TIMEOUT` or `I2C_RETRIES`** (#31). Both
  are adapter-global, so with several providers on one bus the last to open set
  them for all (bread 150 ms and ezo 300 ms on bioreactor-v1). The values live
  on the adapter, so it keeps the last ones set until a reboot; from boot it has
  its driver's values: on i2c-bcm2835 a stuck transfer then times out after the
  i2c core's 1 s default, and a NACK still returns at once.
  **Breaking:** the constructor drops its `timeout_ms` parameter
  (`LinuxI2cBus(bus_path, retry_count)`). That parameter only fed
  `I2C_TIMEOUT`; a provider's `timeout_ms` config keeps any other use it has
  (bread's CRUMBS reply deadline, ezo's job deadlines).

## [0.2.0] — 2026-08-01

### Added

- **Config-schema toolkit, part 1 of 2** (`anolis::provider_sdk_config`, #24):
  the declare-once config declaration model — typed scalar fields (including a
  built-in I2C-address scalar), enums with display titles, inclusive/exclusive
  number bounds, min/max array items, discriminator conditionals,
  `dependentRequired`, parsed-value uniqueness (`x-anolis-unique`)
  — plus a deterministic JSON Schema (draft 2020-12) emitter and the versioned
  `--config-schema` envelope writer (executable profile v1 §2). A separable
  target behind `ANOLIS_PROVIDER_SDK_ENABLE_CONFIG` (default ON).
- **Config-schema toolkit, part 2 of 2** (#24): the YAML validator + typed
  extraction helpers driven by the SAME declaration, completing the no-drift
  guarantee. `validate()` collects ALL errors with dotted paths; scalars type
  by the YAML 1.2 core schema (quoted `"300"` is a string; plain `0x63` is an
  integer; `5.0` satisfies `integer`; no YAML-1.1 `yes`/`on` booleans), so the
  emitted schema is honest about what `--check-config` accepts. Uniqueness
  compares PARSED values (`0x08` vs `"0x08"` collide). The extraction helpers
  (`as_string`/`as_int64`/`as_double`/`as_bool`/`parse_i2c_address`) share the
  resolver so post-validation parsing cannot diverge either. Adds a PUBLIC
  yaml-cpp dependency to the config module (already carried by every
  provider). Contract + rollout guidance: `docs/config-schema.md`.

## [0.1.5] — 2026-07-28

### Added

- **Per-device state override** (`DeviceHealthExtra::state` / `message`, ezo#87):
  a provider may override the SDK's readiness-derived `DeviceHealth.state` (OK
  for a live device, UNREACHABLE for a startup-failed id) to report a live-device
  runtime state the startup report cannot express — `STATE_FAULT` for a failed
  latest read, `STATE_STALE` for an aged sample. Applied after the readiness
  state so the provider wins; disengaged leaves the wire output unchanged.
- **Provider-level health enrichment** (`ProviderHealthExtra` + the defaulted
  `ProviderRuntime::provider_health()` hook, ezo#88): merges provider aggregate
  `metrics` into `ProviderHealth.metrics` alongside the fixed lifecycle keys
  (which win on collision), and applies an optional, escalate-only `state`/
  `message` override (e.g. DEGRADED when a provider's I/O executor is stopped
  even with zero failed devices). Defaulted so existing providers are unaffected.

## [0.1.4] — 2026-07-22

### Added

- **Shared I2C bus seam** (`anolis::provider_sdk_i2c`): a separable, linkable
  module — deliberately *not* part of the hw-agnostic SDK core — providing the
  raw-byte transport abstraction bread and ezo both build their protocols on.
  - `I2cBus` interface: `open`/`close`/`is_open`/`bus_path`, `write`, timed
    `read`, atomic `write_then_read` (repeated-start), `delay_us`, per-address
    `io_stats_for`. Serves both access patterns — ezo's atomic write-then-read
    and bread's query-write + deadline-bounded reply read.
  - `LinuxI2cBus`: real i2c-dev implementation (`I2C_RDWR`, `I2C_TIMEOUT`,
    adapter-global `I2C_RETRIES=0` so every attempt is counted here) —
    consolidates the mechanics previously duplicated in bread's `LinuxTransport`
    and ezo's `LinuxSession`.
  - `FaultInjectingI2cBus`: a decorator wrapping any inner `I2cBus`, driven by a
    `mock://` query-string fault spec (padding, short/corrupt reads, NAK,
    read-fail, timeout, dropout window, latency). Injects over a canned bus in
    mock mode *and* over `LinuxI2cBus` on real hardware (chaos testing).
  - `IoStats` / `IoStatsMap` (`{ok, failed, retried_attempts}`): thread-safe
    per-address transport counters, recorded at the bus layer that knows how
    many attempts an operation took. (anolishq/anolis-provider-sdk#19, #20)
- `docs/metrics.md`: reserved device-health metric vocabulary
  (`io_ok` / `io_failed` / `io_retried_attempts`, protocol-counter
  separation, `last_seen` honesty) so independent providers stay
  consistent without sharing code. Contract-only — the SDK defines key
  semantics; each provider implements them at the layer that performs
  the attempts. Both bread (0.3.3+) and ezo (0.3.1+) already conform
  (anolishq/anolis-provider-ezo#100). (#17, #18)

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

[Unreleased]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.5...v0.2.0
[0.1.5]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/anolishq/anolis-provider-sdk/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/anolishq/anolis-provider-sdk/releases/tag/v0.1.0
