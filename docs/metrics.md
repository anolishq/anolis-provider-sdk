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
| --- | --- |
| `io_ok` | Operations that reached the transport and ultimately succeeded. |
| `io_failed` | Operations that reached the transport and ultimately failed, either after the provider's retry budget was exhausted or on a failure the provider does not retry. |
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
  should document which failure classes its transport layer can see.
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
keys. The values are the provider's last reading of the device's own status
reply (`GET_WATCHDOG`), taken on the provider's I/O path and cached. The
health hook reports that cached reading and its age, and does no device I/O
itself (`ProviderRuntime::device_health` in `runtime.hpp`).

| key | meaning |
| --- | --- |
| `watchdog_armed` | `true`/`false` — whether the firmware watchdog is armed. |
| `watchdog_timeout_ms` | The timeout the device reports it is configured with, in milliseconds. |
| `watchdog_tripped` | `true`/`false` — whether the watchdog has expired since the last clear. Latched only on firmware that advertises the per-type `CLEAR_WATCHDOG_TRIP` capability; on other firmware the next valid command frame, `SET_WATCHDOG` included, clears it. |
| `watchdog_trip_count` | Cumulative trips since device boot. |
| `watchdog_status_age_ms` | Milliseconds from the moment the provider received the status reply the keys above report to the moment it built this health response. |

Rules:

- **Cached, not live.** The SDK calls `device_health` once per device, one
  after another, while it builds a single GetHealth or
  ListDevices(include_health) response. A status query there would add one
  bus round trip per watchdog device to that response, and on a dead bus each
  one blocks until the transport gives up. So the provider reads the status
  where it already does device I/O, stores the reading, and `device_health`
  reports the stored copy.
- **What the device said, not what the provider expects.** The values come
  from the device's status reply. Never derive them from configuration or
  from a command the provider sent: a `SET_WATCHDOG` that succeeded does not
  make `watchdog_armed` `true` until a status reply says so.
- **One reading, with its age.** The five keys describe the same reading:
  whenever the status keys are present, `watchdog_status_age_ms` is present
  too. Measure the age on a monotonic clock, so that a wall-clock step (NTP
  setting the time on a host without a real-time clock, say) cannot make a
  reading look fresher or older than it is.
- **A failed refresh keeps the last reading.** When a later status read fails,
  the provider keeps reporting the last successful reading, and
  `watchdog_status_age_ms` keeps growing. The age is how a reader tells a
  current reading from an old one; how old is too old is the reader's
  decision.
- **Omit, don't fabricate.** When the device lacks the capability, the
  watchdog is not configured, or the provider has no successful reading yet,
  the keys are absent — a reader must treat absence as "unknown", not
  "disarmed".
- Booleans are the strings `true`/`false`; counters follow the same
  cumulative/monotonic convention as the I/O counters, except the reset point
  is the *device* (firmware boot), not the provider process.
  `watchdog_status_age_ms` is a non-negative base-10 integer.
- The four status keys are consumed downstream as typed fields (anolis
  `telemetry-health-timeseries` contract), so their formats are load-bearing.
  (`watchdog_trip_count` is a `uint8` on the BREAD wire and wraps at 255;
  "monotonic" holds between wraps.)

#### Clearing a trip: `clear_watchdog_trip`

The function name `clear_watchdog_trip` is reserved for clearing a latched
trip. A provider whose devices report `watchdog_tripped` and can clear it
declares, on each such device:

- **name** `clear_watchdog_trip`, **no arguments**;
- **category** `CATEGORY_CONFIG`, not `ACTUATE`: clearing a trip is an
  operator's acknowledgement, and it must not move anything. Whatever the trip
  engaged, such as brakes, stays engaged until the operator releases it;
- its effect: `watchdog_tripped` reads `false` on the next health read.
  Health reports the cached reading, so before the call returns the provider
  reads the status back on its I/O path and updates the cache. It does not
  record `false` merely because the clear succeeded.

Anolis's operator panel offers this function on any device that reports
`watchdog_tripped` `true`, with no configuration. Steps that must follow the
clear on a particular machine, such as releasing brakes, are declared by the
machine profile (`operator.trip_recovery`), not by the provider.
Don't use the name for anything else, and don't give it arguments.

### Presence flags

| key | meaning |
| --- | --- |
| `missing` | The provider expected/configured this device but could not bring it up (bread; optional free-text `missing_detail` alongside). |
| `excluded` | The device was excluded from operation after startup failures (ezo). |

Rules:

- **Emitted only when true.** Absence means not-missing / not-excluded; a
  provider must not emit `missing="false"` or `excluded="false"` — an
  explicit false changes timeseries semantics (an explicit point vs. no
  point).
- Value is the string `true`. Consumed downstream as typed booleans (anolis
  `telemetry-health-timeseries` contract), so the format is load-bearing.

### Identity strings

| key | meaning |
| --- | --- |
| `firmware_version` | The firmware revision the device itself reports, as an opaque string (for example `1.0.0`). |

Rules:

- **Omit, don't fabricate.** When the device does not report a revision, the
  key is absent.
- Report what the device says, never a value derived from the provider's own
  build or from configuration.
- The value is a string, and consumers must not parse it as a version. The
  anolis health timeseries stores it as a string field and truncates anything
  over 128 bytes.
- It describes the device's firmware, not the device type's schema, which
  ADPP already carries as `type_version`.

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
The accompanying `message` applies only when the escalation is accepted; a
`message` engaged without a `state` always replaces `ProviderHealth.message`,
including on a startup-degraded provider.
