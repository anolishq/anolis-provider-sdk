#pragma once

// Derive an ADPP `SignalValue.Quality` from a reading's availability and the
// device's last-read freshness (G5). Lifted verbatim from ezo's
// `signal_quality.hpp` (the most complete of the three) so the one quality rule
// applies wherever a `SignalValue` is built. Depends only on the ADPP proto.

#include <cstdint>

#include "anolis/provider_sdk/result.hpp"  // adpp alias
#include "protocol.pb.h"

namespace anolis::provider_sdk {

// Map a reading's flags + age onto an ADPP quality.
//
//   available       the adapter produced a usable sample slot for the signal
//   has_value       the slot carries a concrete value
//   last_read_ok    the device's most recent bus read succeeded
//   age_ms          age of the sample (now - sampled_at), milliseconds
//   stale_after_ms  staleness threshold, milliseconds
//
// Precondition: the caller has a sample for the signal; a missing sample is the
// caller's `QUALITY_UNKNOWN` case.
inline adpp::SignalValue::Quality quality_from(bool available, bool has_value, bool last_read_ok, std::int64_t age_ms,
                                               std::int64_t stale_after_ms) {
    if (!available) {
        return adpp::SignalValue::QUALITY_UNKNOWN;
    }
    if (!last_read_ok) {
        return adpp::SignalValue::QUALITY_FAULT;
    }
    if (age_ms > stale_after_ms) {
        return adpp::SignalValue::QUALITY_STALE;
    }
    if (has_value) {
        return adpp::SignalValue::QUALITY_OK;
    }
    return adpp::SignalValue::QUALITY_UNKNOWN;
}

}  // namespace anolis::provider_sdk
