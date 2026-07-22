#pragma once

/**
 * @file i2c_status.hpp
 * @brief Transport-level status for the shared I2C bus.
 *
 * Deliberately protocol-agnostic: the bus reports whether bytes moved on the
 * wire, not whether a CRUMBS/EZO payload decoded. Protocol decode failures stay
 * in the provider on top of the bus. The code set is the union of what bread's
 * `SessionErrorCode` and ezo's `StatusCode` need at the transport layer.
 */

#include <string>
#include <utility>

namespace anolis::provider_sdk::i2c {

enum class I2cError {
    Ok = 0,
    InvalidArgument,  // caller error (e.g. zero-length transaction)
    NotOpen,          // bus used before open() / after close()
    OpenFailed,       // could not open the bus device node
    WriteFailed,      // write phase failed (NAK / bus error)
    ReadFailed,       // read phase failed (NAK / bus error)
    Timeout,          // operation did not complete within its deadline
    BusError,         // other transport failure
};

struct I2cStatus {
    I2cError code = I2cError::Ok;
    std::string message;
    int native_errno = 0;  // errno at the failing syscall, when applicable

    /** @brief True when the operation succeeded. */
    explicit operator bool() const { return code == I2cError::Ok; }

    static I2cStatus ok() { return I2cStatus{}; }

    static I2cStatus failure(I2cError code, std::string message, int native_errno = 0) {
        return I2cStatus{code, std::move(message), native_errno};
    }
};

inline const char *to_string(I2cError code) {
    switch (code) {
        case I2cError::Ok:
            return "Ok";
        case I2cError::InvalidArgument:
            return "InvalidArgument";
        case I2cError::NotOpen:
            return "NotOpen";
        case I2cError::OpenFailed:
            return "OpenFailed";
        case I2cError::WriteFailed:
            return "WriteFailed";
        case I2cError::ReadFailed:
            return "ReadFailed";
        case I2cError::Timeout:
            return "Timeout";
        case I2cError::BusError:
            return "BusError";
    }
    return "Unknown";
}

}  // namespace anolis::provider_sdk::i2c
