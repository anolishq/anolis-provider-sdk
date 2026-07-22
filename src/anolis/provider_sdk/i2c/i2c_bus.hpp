#pragma once

/**
 * @file i2c_bus.hpp
 * @brief Shared raw-byte I2C transport seam for hardware providers.
 *
 * The single hardware abstraction that bread and ezo both build their protocol
 * on: raw I2C byte transactions, protocol-agnostic. It offers both access
 * patterns the two providers need:
 *
 * - `write_then_read` — one atomic transaction preserving repeated-start (ezo's
 *   EZO-command pattern).
 * - `write` + timed `read` — a query write followed by a separate, deadline-
 *   bounded reply read (bread's CRUMBS request/response pattern, where the
 *   device needs processing time between phases).
 *
 * Implementations own the platform mechanics and the retry/timeout/errno
 * handling; per-address IoStats are recorded here, at the layer that knows how
 * many attempts an operation actually took.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "anolis/provider_sdk/i2c/i2c_status.hpp"
#include "anolis/provider_sdk/i2c/io_stats.hpp"

namespace anolis::provider_sdk::i2c {

class I2cBus {
public:
    virtual ~I2cBus() = default;

    /** @brief Acquire the underlying bus resources. */
    virtual I2cStatus open() = 0;

    /** @brief Release the underlying bus resources. */
    virtual void close() = 0;

    /** @brief Whether the bus is currently open. */
    virtual bool is_open() const = 0;

    /** @brief The configured bus path this instance drives. */
    virtual const std::string &bus_path() const = 0;

    /** @brief Write @p tx_len bytes to @p address (query phase). */
    virtual I2cStatus write(uint8_t address, const uint8_t *tx_data, size_t tx_len) = 0;

    /**
     * @brief Read up to @p rx_len bytes from @p address within @p timeout_us.
     *
     * @param rx_received when non-null, set to the number of bytes returned.
     */
    virtual I2cStatus read(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received,
                           uint32_t timeout_us) = 0;

    /**
     * @brief One atomic write-then-read transaction (repeated-start preserved).
     *
     * @param rx_received when non-null, set to the number of bytes read.
     */
    virtual I2cStatus write_then_read(uint8_t address, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data,
                                      size_t rx_len, size_t *rx_received) = 0;

    /** @brief Sleep for a bus-appropriate inter-phase delay. */
    virtual void delay_us(uint32_t delay_us) = 0;

    /** @brief Cumulative per-address transport I/O counters. */
    virtual IoStats io_stats_for(uint8_t address) const = 0;
};

}  // namespace anolis::provider_sdk::i2c
