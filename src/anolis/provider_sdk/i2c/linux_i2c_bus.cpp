#include "anolis/provider_sdk/i2c/linux_i2c_bus.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace anolis::provider_sdk::i2c {

LinuxI2cBus::LinuxI2cBus(std::string bus_path, int timeout_ms, int retry_count)
    : bus_path_(std::move(bus_path)), timeout_ms_(std::max(timeout_ms, 1)), retry_count_(std::max(retry_count, 0)) {}

LinuxI2cBus::~LinuxI2cBus() { close(); }

const std::string &LinuxI2cBus::bus_path() const { return bus_path_; }
bool LinuxI2cBus::is_open() const { return opened_; }
IoStats LinuxI2cBus::io_stats_for(uint8_t address) const { return io_stats_.stats_for(address); }

void LinuxI2cBus::delay_us(uint32_t delay_us) {
    if (delay_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    }
}

I2cStatus LinuxI2cBus::open() {
    if (opened_) {
        return I2cStatus::ok();
    }
    if (bus_path_.empty()) {
        return I2cStatus::failure(I2cError::InvalidArgument, "bus_path is empty");
    }
#if defined(__linux__)
    fd_ = ::open(bus_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        const int saved = errno;
        return I2cStatus::failure(I2cError::OpenFailed, "failed to open " + bus_path_ + ": " + std::strerror(saved),
                                  saved);
    }
    // I2C_TIMEOUT unit is 10ms.
    (void)::ioctl(fd_, I2C_TIMEOUT, std::max(1, timeout_ms_ / 10));
    // Disable the kernel's own I2C_RETRIES loop (adapter-global, EAGAIN-only) so
    // every attempt is issued and counted here. See ezo#100 for the full note.
    (void)::ioctl(fd_, I2C_RETRIES, 0);
    opened_ = true;
    return I2cStatus::ok();
#else
    return I2cStatus::failure(I2cError::BusError, "LinuxI2cBus is only available on Linux builds");
#endif
}

void LinuxI2cBus::close() {
#if defined(__linux__)
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
#endif
    opened_ = false;
}

#if defined(__linux__)
namespace {
// One I2C_RDWR of the given messages. Returns true on success; sets errno.
bool rdwr(int fd, struct i2c_msg *msgs, int nmsgs) {
    struct i2c_rdwr_ioctl_data data;
    data.msgs = msgs;
    data.nmsgs = static_cast<__u32>(nmsgs);
    return ::ioctl(fd, I2C_RDWR, &data) >= 0;
}
}  // namespace
#endif

I2cStatus LinuxI2cBus::write(uint8_t address, const uint8_t *tx_data, size_t tx_len) {
    if (!opened_) {
        return I2cStatus::failure(I2cError::NotOpen, "bus not open");
    }
    if (tx_len == 0) {
        return I2cStatus::failure(I2cError::InvalidArgument, "write requires tx_len>0");
    }
#if defined(__linux__)
    struct i2c_msg msg;
    msg.addr = address;
    msg.flags = 0;
    msg.len = static_cast<__u16>(tx_len);
    msg.buf = const_cast<__u8 *>(reinterpret_cast<const __u8 *>(tx_data));

    const int max_attempts = std::max(retry_count_ + 1, 1);
    int attempts = 0;
    int last_errno = 0;
    for (int i = 0; i < max_attempts; ++i) {
        ++attempts;
        if (rdwr(fd_, &msg, 1)) {
            io_stats_.record(address, true, attempts);
            return I2cStatus::ok();
        }
        last_errno = errno;
        if (last_errno != EINTR && last_errno != EAGAIN) {
            break;
        }
    }
    io_stats_.record(address, false, attempts);
    return I2cStatus::failure(I2cError::WriteFailed, "write failed on " + bus_path_ + ": " + std::strerror(last_errno),
                              last_errno);
#else
    (void)address;
    (void)tx_data;
    return I2cStatus::failure(I2cError::BusError, "LinuxI2cBus is only available on Linux builds");
#endif
}

I2cStatus LinuxI2cBus::read(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received,
                            uint32_t timeout_us) {
    if (!opened_) {
        return I2cStatus::failure(I2cError::NotOpen, "bus not open");
    }
    if (rx_len == 0) {
        return I2cStatus::failure(I2cError::InvalidArgument, "read requires rx_len>0");
    }
#if defined(__linux__)
    struct i2c_msg msg;
    msg.addr = address;
    msg.flags = I2C_M_RD;
    msg.len = static_cast<__u16>(rx_len);
    msg.buf = reinterpret_cast<__u8 *>(rx_data);

    // Poll the reply until the device answers or the deadline passes: the
    // request/response (CRUMBS) pattern needs device processing time between the
    // query write and this read, during which the device NAKs. Any errno in the
    // window is treated as "not ready yet" and retried — a truly absent device
    // would have NAK'd the preceding query write. Mapping specific hard errnos
    // (ENXIO/ENODEV) to an early ReadFailed rather than consuming the full
    // timeout is a bench-validation refinement for the bread migration
    // (anolis-provider-sdk#19), where real device NAK timing is observable.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    int attempts = 0;
    int last_errno = 0;
    while (true) {
        ++attempts;
        if (rdwr(fd_, &msg, 1)) {
            if (rx_received != nullptr) {
                *rx_received = rx_len;
            }
            io_stats_.record(address, true, attempts);
            return I2cStatus::ok();
        }
        last_errno = errno;
        if (std::chrono::steady_clock::now() >= deadline) {
            io_stats_.record(address, false, attempts);
            return I2cStatus::failure(I2cError::Timeout, "read timed out on " + bus_path_, last_errno);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
#else
    (void)address;
    (void)rx_data;
    (void)rx_received;
    (void)timeout_us;
    return I2cStatus::failure(I2cError::BusError, "LinuxI2cBus is only available on Linux builds");
#endif
}

I2cStatus LinuxI2cBus::write_then_read(uint8_t address, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data,
                                       size_t rx_len, size_t *rx_received) {
    if (!opened_) {
        return I2cStatus::failure(I2cError::NotOpen, "bus not open");
    }
    if (tx_len == 0 && rx_len == 0) {
        return I2cStatus::failure(I2cError::InvalidArgument, "write_then_read requires tx_len>0 or rx_len>0");
    }
#if defined(__linux__)
    // One I2C_RDWR so the write+read preserves repeated-start.
    struct i2c_msg msgs[2];
    int n = 0;
    if (tx_len > 0) {
        msgs[n].addr = address;
        msgs[n].flags = 0;
        msgs[n].len = static_cast<__u16>(tx_len);
        msgs[n].buf = const_cast<__u8 *>(reinterpret_cast<const __u8 *>(tx_data));
        ++n;
    }
    if (rx_len > 0) {
        msgs[n].addr = address;
        msgs[n].flags = I2C_M_RD;
        msgs[n].len = static_cast<__u16>(rx_len);
        msgs[n].buf = reinterpret_cast<__u8 *>(rx_data);
        ++n;
    }

    const int max_attempts = std::max(retry_count_ + 1, 1);
    int attempts = 0;
    int last_errno = 0;
    for (int i = 0; i < max_attempts; ++i) {
        ++attempts;
        if (rdwr(fd_, msgs, n)) {
            if (rx_received != nullptr) {
                *rx_received = rx_len;
            }
            io_stats_.record(address, true, attempts);
            return I2cStatus::ok();
        }
        last_errno = errno;
        if (last_errno != EINTR && last_errno != EAGAIN) {
            break;
        }
    }
    io_stats_.record(address, false, attempts);
    return I2cStatus::failure(I2cError::BusError, "I2C_RDWR failed on " + bus_path_ + ": " + std::strerror(last_errno),
                              last_errno);
#else
    (void)address;
    (void)tx_data;
    (void)tx_len;
    (void)rx_data;
    (void)rx_len;
    (void)rx_received;
    return I2cStatus::failure(I2cError::BusError, "LinuxI2cBus is only available on Linux builds");
#endif
}

}  // namespace anolis::provider_sdk::i2c
