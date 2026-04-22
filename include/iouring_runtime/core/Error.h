#pragma once

#include <cstdint>

namespace iouring_runtime::core {

enum class CoreError : uint8_t {
    kInvalidArgument,
    kResourceExhausted,
    kAlreadyExists,
    kNotFound,
};

namespace ring {
enum class RingError : uint8_t {
    kSetupFailed,
    kSubmissionFailed,
    kBufferRegistrationFailed,
};
} // namespace ring

namespace io {
enum class IoError : uint8_t {
    kConnectionRefused,
    kDisconnected,
    kTimeout,
    kSendFailed,
    kMalformedPacket,
    kBindFailed,
    kListenFailed,
};
} // namespace io

} // namespace iouring_runtime::core
