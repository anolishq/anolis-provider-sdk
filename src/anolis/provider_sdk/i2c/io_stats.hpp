#pragma once

/**
 * @file io_stats.hpp
 * @brief Per-address transport I/O counters, shared across I2C providers.
 *
 * These are the `io_ok` / `io_failed` / `io_retried_attempts` counters the
 * providers report on `DeviceHealth` and the runtime ingests to InfluxDB
 * (anolishq/anolis#203). Counting lives at the bus layer — the only place that
 * knows how many transport attempts an operation actually took. Semantics are
 * fixed by anolis-provider-sdk docs/metrics.md:
 *
 * - `ok`               operations that reached the transport and succeeded.
 * - `failed`           operations that reached the transport and ultimately
 *                      failed after the retry budget was exhausted.
 * - `retried_attempts` every attempt beyond an operation's first, including
 *                      attempts of operations that eventually succeeded.
 *
 * Counters are cumulative for the process lifetime and monotonic.
 */

#include <cstdint>
#include <map>
#include <mutex>

namespace anolis::provider_sdk::i2c {

struct IoStats {
    uint64_t ok = 0;
    uint64_t failed = 0;
    uint64_t retried_attempts = 0;
};

/**
 * @brief Thread-safe per-address IoStats accumulator.
 *
 * A bus is shared across the poller, the health-snapshot task, and HTTP
 * threads, so record()/stats_for() are mutex-guarded.
 */
class IoStatsMap {
public:
    /**
     * @brief Record one completed operation for @p address.
     *
     * @param ok            whether the operation ultimately succeeded.
     * @param attempts_made total transport attempts this operation took (>= 1);
     *                      `attempts_made - 1` is added to retried_attempts.
     */
    void record(uint8_t address, bool ok, int attempts_made) {
        const uint64_t retries = attempts_made > 1 ? static_cast<uint64_t>(attempts_made - 1) : 0;
        std::lock_guard<std::mutex> lock(mutex_);
        IoStats &s = stats_[address];
        if (ok) {
            ++s.ok;
        } else {
            ++s.failed;
        }
        s.retried_attempts += retries;
    }

    IoStats stats_for(uint8_t address) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = stats_.find(address);
        return it == stats_.end() ? IoStats{} : it->second;
    }

private:
    mutable std::mutex mutex_;
    std::map<uint8_t, IoStats> stats_;
};

}  // namespace anolis::provider_sdk::i2c
