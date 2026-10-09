#pragma once

/**
 * @file linux_i2c_bus.hpp
 * @brief Real i2c-dev implementation of I2cBus.
 *
 * Consolidates the I2C mechanics previously duplicated in bread's
 * `LinuxTransport` and ezo's `LinuxSession`: open, an I2C_RDWR-based atomic
 * write_then_read, a retry-budgeted write, and a deadline-bounded poll read for
 * the request/response (CRUMBS) pattern.
 *
 * It sets no adapter-global kernel state. I2C_TIMEOUT and I2C_RETRIES apply to
 * every process on the adapter, so with several providers on one bus the last
 * to open would set them for all (#31). The values live on the adapter and
 * outlast the process that set them, so an adapter keeps whatever was last set
 * until its driver is rebound (a reboot); from boot it has the driver's values.
 * Then the i2c core times a stuck transfer out after its default (1 s when the
 * driver sets none, as i2c-bcm2835 does); a NACK still returns at once. On an
 * adapter whose driver sets retries, the kernel repeats an arbitration-lost
 * (EAGAIN) transfer inside one attempt counted here; i2c-bcm2835 sets none.
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
     * @param retry_count  userspace retry budget for write / write_then_read.
     */
    LinuxI2cBus(std::string bus_path, int retry_count);
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
    int retry_count_;
    int fd_ = -1;
    bool opened_ = false;
    IoStatsMap io_stats_;
};

}  // namespace anolis::provider_sdk::i2c
