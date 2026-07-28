# Device health metrics vocabulary

Providers report per-device metrics through `DeviceHealthExtra.metrics`
(`runtime.hpp`), a free-form `string -> string` map. This document reserves key
names and pins their semantics so independent providers stay consistent
without sharing code: the SDK defines *what the keys mean*; each provider
implements them at whatever layer is honest for its transport. Consumers (the
anolis runtime's `/v0/providers/health` passthrough, dashboards) treat the
values as opaque strings — except where a key below is noted as consumed
typed: the anolis health-timeseries ingestion parses those reserved keys
strictly (base-10 integers; exactly `true`/`false` booleans), and a value in
any other format is silently dropped from the timeseries.

Conformance is per-key: a provider that emits a reserved key MUST follow its
contract below. Providers are free to emit additional provider-specific keys;
they must not redefine reserved ones.

## Reserved keys

### Transport I/O counters

| key | meaning |
|---|---|
| `io_ok` | Operations that reached the transport and ultimately succeeded. |
| `io_failed` | Operations that reached the transport and ultimately failed, after the provider's retry budget was exhausted. |
| `io_retried_attempts` | Every attempt beyond an operation's first — including attempts whose operation eventually succeeded. |

Contract:

- **Counted at the layer that performs the attempts.** Attempt counts are only
  honest where the retries happen; a provider that delegates retries to an
  environment it cannot observe (e.g. kernel `I2C_RETRIES`) must first make
  the attempts observable, not report approximations.
- **The masked-retry property is the point.** `io_retried_attempts` exists so
  intermittent transport trouble that retries absorb stays visible instead of
  disappearing into `io_ok`. (Motivating incident: a ~30% transaction-failure
  rate hidden for weeks behind host retries, feastorg/Slice_DCMT#3.)
- **Transport-level scope.** These counters measure whether the transport
  operation succeeded, judged by everything the provider's transport layer can
  see. Where framing/decode is part of the transport (bread: a CRUMBS frame
  that fails CRC or decode is a transport failure), garbage lands in
  `io_failed`; where the transport sees only electrical success (ezo: a raw
  I2C transaction), a garbage or not-ready payload counts in `io_ok` and the
  failure surfaces in protocol-level counters (see below). Each provider
  documents which failure classes its transport layer can see.
- **Operations that never reach the transport are not I/O.** Validation
  failures, not-open errors, and queue timeouts observed above the transport
  are excluded.
- **Granularity is provider-defined and magnitudes are NOT comparable across
  providers.** bread counts logical session operations (one `query_read` = 1);
  ezo counts raw I2C transactions (one sample = several). The shared contract
  is the key vocabulary and the properties above, never the magnitudes.
- Counters are cumulative for the provider process lifetime, monotonic, and
  reset on process restart.

### Protocol-level counters

Where a provider distinguishes transport success from payload validity, the
protocol-level outcome belongs in separate keys (e.g. ezo's
`sample_success_count` / `sample_failure_count` / `call_success_count` /
`call_failure_count`). Do not fold protocol failures into `io_failed`.

### Command-watchdog state

Providers whose devices carry a firmware command watchdog (BREAD
`SET_WATCHDOG`/`GET_WATCHDOG` vocabulary; bread today) report it under these
keys, sourced from a live status query — never from cached provider state:

| key | meaning |
|---|---|
| `watchdog_armed` | `true`/`false` — whether the firmware watchdog is armed. |
| `watchdog_timeout_ms` | Configured timeout in milliseconds. |
| `watchdog_tripped` | `true`/`false` — latched trip indicator. |
| `watchdog_trip_count` | Cumulative trips since device boot. |

Rules:

- **Omit, don't fabricate.** When the device lacks the capability, the
  watchdog is not configured, or the live query fails, the keys are absent —
  a reader must treat absence as "unknown", not "disarmed".
- Booleans are the strings `true`/`false`; counters follow the same
  cumulative/monotonic convention as the I/O counters, except the reset point
  is the *device* (firmware boot), not the provider process.
- These keys are consumed downstream as typed fields (anolis
  `telemetry-health-timeseries` contract), so their formats are load-bearing.
  (`watchdog_trip_count` is a `uint8` on the BREAD wire and wraps at 255;
  "monotonic" holds between wraps.)

### Presence flags

| key | meaning |
|---|---|
| `missing` | The provider expected/configured this device but could not bring it up (bread; optional free-text `missing_detail` alongside). |
| `excluded` | The device was excluded from operation after startup failures (ezo). |

Rules:

- **Emitted only when true.** Absence means not-missing / not-excluded; a
  provider must not emit `missing="false"` or `excluded="false"` — an
  explicit false changes timeseries semantics (an explicit point vs. no
  point).
- Value is the string `true`. Consumed downstream as typed booleans (anolis
  `telemetry-health-timeseries` contract), so the format is load-bearing.

### `last_seen` (structured field, not a metrics key)

`DeviceHealthExtra.last_seen` is set only from a **genuine success** — a real
transport contact or a real sample — and never fabricated or defaulted. A
device with no successful contact reports no `last_seen`. (In an explicit
mock/simulation mode, "genuine" means genuine within the simulation:
simulated samples may set it.)

## Existing shared idioms (non-normative)

These are conventions the current providers happen to share; they are
recommendations, not contracts — providers may diverge where their domain
requires it:

- Readiness diagnostics: `init_time_ms`, `ready`, `startup_message`,
  `bus_path`.
- Config vocabulary: `hardware.{bus_path, query_delay_us, timeout_ms,
  retry_count}` with unknown-key rejection.
- Identity metrics: `address` (`0xNN`; hex case currently differs per
  provider) and a device-kind key (`type` in ezo, `type_id` in bread).

## Provider-level metrics (`ProviderHealthExtra.metrics`)

Distinct from the per-device metrics above, a provider may report aggregate
metrics for the whole provider through `ProviderHealthExtra.metrics` (the
defaulted `ProviderRuntime::provider_health()` hook), merged by the SDK into
`ProviderHealth.metrics`.

The SDK always emits a fixed lifecycle set — `impl`, `startup_policy`,
`startup_configured_devices`, `startup_initialized_devices`,
`startup_failed_devices` — and these **win on collision**: a provider MUST NOT
redefine them. Provider-specific aggregate keys (e.g. `excluded_devices`,
`call_success_total`, `call_failure_total`, `excluded_reason.<id>`) are free-form
and follow the same "opaque string, base-10 integer where a consumer parses it"
convention as the device metrics above.

`ProviderHealthExtra.state` / `message` are an optional, **escalate-only**
override of `ProviderHealth.state`: a provider engages them to report a state
the startup report cannot (e.g. `STATE_DEGRADED` when its I/O executor is
stopped with zero failed devices) and leaves them disengaged when healthy, so
the override never un-degrades a startup-degraded provider.
