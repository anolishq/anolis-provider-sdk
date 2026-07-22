#pragma once

/**
 * @file linux_i2c_bus.hpp
 * @brief Real i2c-dev implementation of I2cBus.
 *
 * Consolidates the I2C mechanics previously duplicated in bread's
 * `LinuxTransport` and ezo's `LinuxSession`: open + I2C_TIMEOUT +
 * adapter-global I2C_RETRIES=0 (so every attempt is counted here, ezo#100),
 * an I2C_RDWR-based atomic write_then_read, a retry-budgeted write, and a
 * deadline-bounded poll read for the request/response (CRUMBS) pattern.
 */

#include <cstdint>
#include <string>

#include "anolis/provider_sdk/i2c/i2c_bus.hpp"
#include "anolis/provider_sdk/i2c/io_stats.hpp"

namespace anolis::provider_sdk::i2c {

class LinuxI2cBus final : public I2cBus {
public:
    /**
     * @param bus_path     device node, e.g. "/dev/i2c-1".
     * @param timeout_ms   per-transaction I2C_TIMEOUT (kernel-side).
     * @param retry_count  userspace retry budget for write / write_then_read.
     */
    LinuxI2cBus(std::string bus_path, int timeout_ms, int retry_count);
    ~LinuxI2cBus() override;

    LinuxI2cBus(const LinuxI2cBus &) = delete;
    LinuxI2cBus &operator=(const LinuxI2cBus &) = delete;

    I2cStatus open() override;
    void close() override;
    bool is_open() const override;
    const std::string &bus_path() const override;

    I2cStatus write(uint8_t address, const uint8_t *tx_data, size_t tx_len) override;
    I2cStatus read(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received, uint32_t timeout_us) override;
    I2cStatus write_then_read(uint8_t address, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data, size_t rx_len,
                              size_t *rx_received) override;
    void delay_us(uint32_t delay_us) override;
    IoStats io_stats_for(uint8_t address) const override;

private:
    std::string bus_path_;
    int timeout_ms_;
    int retry_count_;
    int fd_ = -1;
    bool opened_ = false;
    IoStatsMap io_stats_;
};

}  // namespace anolis::provider_sdk::i2c
