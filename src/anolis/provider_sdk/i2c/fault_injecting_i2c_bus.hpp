#pragma once

/**
 * @file fault_injecting_i2c_bus.hpp
 * @brief An I2cBus decorator that injects deterministic transport faults.
 *
 * Wraps any inner `I2cBus` and applies configured wire-level faults on top —
 * the failure surface real hardware exhibits but `mock://` and the sim never
 * do (anolishq/anolis#99, bread#97). Because it decorates *any* inner bus it
 * serves both mock mode (`FaultInjectingI2cBus(<provider canned bus>)`) and
 * chaos-on-real-hardware (`FaultInjectingI2cBus(LinuxI2cBus)`).
 *
 * Faults are **count-based and deterministic** (every-Nth, after-N) so tests
 * are reproducible; the spec is parsed from `mock://` query params, e.g.
 *   mock://bus?pad=8&nak_every=3&read_fail_every=5&timeout_every=7&
 *             short_every=4&corrupt_every=6&drop_after=100&drop_for=50&latency_us=500
 *
 * Fault modes:
 *   pad=N / pad_byte=0xNN  append N padding bytes (default 0xFF) to every read
 *   short_every=N          the Nth read returns one fewer byte (short read)
 *   corrupt_every=N        the Nth read flips a payload byte (bad CRC/garbage)
 *   nak_every=N            the Nth write / write phase fails WriteFailed
 *   read_fail_every=N      the Nth read / read phase fails ReadFailed
 *   timeout_every=N        the Nth operation fails Timeout
 *   drop_after=N drop_for=M  after N ops, the next M ops fail BusError, then recover
 *   latency_us=N           add N microseconds to every operation
 */

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "anolis/provider_sdk/i2c/i2c_bus.hpp"

namespace anolis::provider_sdk::i2c {

struct FaultSpec {
    uint32_t pad = 0;
    uint8_t pad_byte = 0xFF;
    uint32_t short_every = 0;
    uint32_t corrupt_every = 0;
    uint32_t nak_every = 0;
    uint32_t read_fail_every = 0;
    uint32_t timeout_every = 0;
    uint32_t drop_after = 0;
    uint32_t drop_for = 0;
    uint32_t latency_us = 0;

    /** @brief Whether any fault mode is active (else the decorator is a pass-through). */
    bool any() const {
        return pad || short_every || corrupt_every || nak_every || read_fail_every || timeout_every || drop_for ||
               latency_us;
    }

    /**
     * @brief Parse a `key=value&...` query string. Unknown keys are ignored;
     *        malformed values leave that field at its default.
     */
    static FaultSpec parse(const std::string &query);
};

/** @brief Split a `mock://path?query` bus path into {path-without-query, query}. */
std::pair<std::string, std::string> split_bus_query(const std::string &bus_path);

class FaultInjectingI2cBus final : public I2cBus {
public:
    FaultInjectingI2cBus(std::unique_ptr<I2cBus> inner, FaultSpec spec);

    I2cStatus open() override;
    void close() override;
    bool is_open() const override;
    const std::string &bus_path() const override;

    I2cStatus write(uint8_t address, const uint8_t *tx_data, size_t tx_len) override;
    I2cStatus read(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received, uint32_t timeout_us) override;
    I2cStatus write_then_read(uint8_t address, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data, size_t rx_len,
                              size_t *rx_received) override;
    void delay_us(uint32_t delay_us) override;

    /**
     * @brief Combined io_stats: the inner bus's real attempts plus the injected
     * transport failures this decorator short-circuited.
     *
     * An injected NAK/timeout/dropout is a simulated *transport* failure, so it
     * counts as io_failed here (docs/metrics.md) — the phase-3 health-metric
     * tie-in relies on that. Read mutations (pad/short/corrupt) are NOT failures:
     * the transport succeeded and returned bad *data*, so they stay io_ok and
     * surface as the provider's protocol/decode failure instead.
     */
    IoStats io_stats_for(uint8_t address) const override;

private:
    // Returns a failure status to inject for this op, or Ok to proceed. Advances
    // the counters, applies latency, and records any injected transport failure
    // into fault_stats_. `write_phase`/`read_phase` select which every-N failures
    // are eligible (write_then_read sets both).
    I2cStatus pre_op(uint8_t address, bool write_phase, bool read_phase);
    // Mutate a successful read's output per pad/short/corrupt.
    void mutate_read(uint8_t *rx_data, size_t rx_len, size_t *rx_received);

    std::unique_ptr<I2cBus> inner_;
    FaultSpec spec_;
    IoStatsMap fault_stats_;    // injected transport failures, added to inner's stats
    uint64_t op_count_ = 0;     // every operation
    uint64_t write_count_ = 0;  // write() + write phase of write_then_read()
    uint64_t read_count_ = 0;   // read() + read phase of write_then_read()
};

}  // namespace anolis::provider_sdk::i2c
