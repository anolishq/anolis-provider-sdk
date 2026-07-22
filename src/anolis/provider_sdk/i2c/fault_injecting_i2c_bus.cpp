#include "anolis/provider_sdk/i2c/fault_injecting_i2c_bus.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

namespace anolis::provider_sdk::i2c {

namespace {

// Parse an unsigned value; supports 0x-prefixed hex. Returns false on garbage,
// leaving `out` untouched so the field keeps its default.
bool parse_u32(const std::string &value, uint32_t &out) {
    if (value.empty()) {
        return false;
    }
    if (value[0] == '-') {  // strtoul silently wraps a negative; reject it.
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long v = std::strtoul(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0' || errno == ERANGE || v > 0xFFFFFFFFUL) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

}  // namespace

FaultSpec FaultSpec::parse(const std::string &query) {
    FaultSpec spec;
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        pos = amp == std::string::npos ? query.size() : amp + 1;

        const size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = pair.substr(0, eq);
        const std::string value = pair.substr(eq + 1);

        uint32_t v = 0;
        if (key == "pad") {
            parse_u32(value, spec.pad);
        } else if (key == "pad_byte") {
            if (parse_u32(value, v)) {
                spec.pad_byte = static_cast<uint8_t>(v);
            }
        } else if (key == "short_every") {
            parse_u32(value, spec.short_every);
        } else if (key == "corrupt_every") {
            parse_u32(value, spec.corrupt_every);
        } else if (key == "nak_every") {
            parse_u32(value, spec.nak_every);
        } else if (key == "read_fail_every") {
            parse_u32(value, spec.read_fail_every);
        } else if (key == "timeout_every") {
            parse_u32(value, spec.timeout_every);
        } else if (key == "drop_after") {
            parse_u32(value, spec.drop_after);
        } else if (key == "drop_for") {
            parse_u32(value, spec.drop_for);
        } else if (key == "latency_us") {
            parse_u32(value, spec.latency_us);
        }
        // Unknown keys ignored.
    }
    return spec;
}

std::pair<std::string, std::string> split_bus_query(const std::string &bus_path) {
    const size_t q = bus_path.find('?');
    if (q == std::string::npos) {
        return {bus_path, ""};
    }
    return {bus_path.substr(0, q), bus_path.substr(q + 1)};
}

FaultInjectingI2cBus::FaultInjectingI2cBus(std::unique_ptr<I2cBus> inner, FaultSpec spec)
    : inner_(std::move(inner)), spec_(spec) {}

I2cStatus FaultInjectingI2cBus::open() { return inner_->open(); }
void FaultInjectingI2cBus::close() { inner_->close(); }
bool FaultInjectingI2cBus::is_open() const { return inner_->is_open(); }
const std::string &FaultInjectingI2cBus::bus_path() const { return inner_->bus_path(); }
void FaultInjectingI2cBus::delay_us(uint32_t delay_us) { inner_->delay_us(delay_us); }

IoStats FaultInjectingI2cBus::io_stats_for(uint8_t address) const {
    const IoStats real = inner_->io_stats_for(address);
    const IoStats injected = fault_stats_.stats_for(address);
    return IoStats{real.ok + injected.ok, real.failed + injected.failed,
                   real.retried_attempts + injected.retried_attempts};
}

I2cStatus FaultInjectingI2cBus::pre_op(uint8_t address, bool write_phase, bool read_phase) {
    ++op_count_;
    if (write_phase) {
        ++write_count_;
    }
    if (read_phase) {
        ++read_count_;
    }

    if (spec_.latency_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(spec_.latency_us));
    }

    I2cStatus fault = I2cStatus::ok();
    // Dropout window: ops (drop_after, drop_after + drop_for] fail, then recover.
    if (spec_.drop_for > 0 && op_count_ > spec_.drop_after && op_count_ <= spec_.drop_after + spec_.drop_for) {
        fault = I2cStatus::failure(I2cError::BusError, "fault: device dropped out");
    } else if (spec_.timeout_every > 0 && op_count_ % spec_.timeout_every == 0) {
        fault = I2cStatus::failure(I2cError::Timeout, "fault: injected timeout");
    } else if (write_phase && spec_.nak_every > 0 && write_count_ % spec_.nak_every == 0) {
        fault = I2cStatus::failure(I2cError::WriteFailed, "fault: injected NAK");
    } else if (read_phase && spec_.read_fail_every > 0 && read_count_ % spec_.read_fail_every == 0) {
        fault = I2cStatus::failure(I2cError::ReadFailed, "fault: injected read failure");
    }

    // An injected fault is a simulated transport failure — count it as io_failed
    // so the health-metric pipeline sees it (a short-circuited op never reaches
    // the inner bus, so the inner would otherwise never record it).
    if (!fault) {
        fault_stats_.record(address, false, 1);
    }
    return fault;
}

void FaultInjectingI2cBus::mutate_read(uint8_t *rx_data, size_t rx_len, size_t *rx_received) {
    if (rx_data == nullptr || rx_received == nullptr || *rx_received == 0) {
        return;
    }
    // Corrupt: flip the first payload byte (breaks CRC / yields garbage).
    if (spec_.corrupt_every > 0 && read_count_ % spec_.corrupt_every == 0) {
        rx_data[0] = static_cast<uint8_t>(rx_data[0] ^ 0xFF);
    }
    // Short read: return one fewer byte (truncated frame).
    if (spec_.short_every > 0 && read_count_ % spec_.short_every == 0 && *rx_received > 0) {
        --(*rx_received);
    }
    // Padding: append pad_byte after the frame, as a real i2c-dev read does when
    // the device replies shorter than the requested length (bread#97).
    if (spec_.pad > 0) {
        const size_t base = *rx_received;
        const size_t add = std::min<size_t>(spec_.pad, rx_len > base ? rx_len - base : 0);
        for (size_t i = 0; i < add; ++i) {
            rx_data[base + i] = spec_.pad_byte;
        }
        *rx_received = base + add;
    }
}

I2cStatus FaultInjectingI2cBus::write(uint8_t address, const uint8_t *tx_data, size_t tx_len) {
    I2cStatus fault = pre_op(address, /*write_phase=*/true, /*read_phase=*/false);
    if (!fault) {
        return fault;
    }
    return inner_->write(address, tx_data, tx_len);
}

I2cStatus FaultInjectingI2cBus::read(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received,
                                     uint32_t timeout_us) {
    I2cStatus fault = pre_op(address, /*write_phase=*/false, /*read_phase=*/true);
    if (!fault) {
        return fault;
    }
    I2cStatus status = inner_->read(address, rx_data, rx_len, rx_received, timeout_us);
    if (status) {
        mutate_read(rx_data, rx_len, rx_received);
    }
    return status;
}

I2cStatus FaultInjectingI2cBus::write_then_read(uint8_t address, const uint8_t *tx_data, size_t tx_len,
                                                uint8_t *rx_data, size_t rx_len, size_t *rx_received) {
    I2cStatus fault = pre_op(address, /*write_phase=*/true, /*read_phase=*/true);
    if (!fault) {
        return fault;
    }
    I2cStatus status = inner_->write_then_read(address, tx_data, tx_len, rx_data, rx_len, rx_received);
    if (status) {
        mutate_read(rx_data, rx_len, rx_received);
    }
    return status;
}

}  // namespace anolis::provider_sdk::i2c
