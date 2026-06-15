#pragma once

#include <cstdint>

namespace iouring::core {

enum class CoreError : uint8_t {
    kInvalidArgument,
    kResourceExhausted,
    kAlreadyExists,
    kNotFound,
};

} // namespace iouring::core

namespace iouring::event {

enum class RingError : uint8_t {
    kSetupFailed,
    kSubmissionFailed,
    kBufferRegistrationFailed,
};

} // namespace iouring::event

namespace iouring::net {

enum class IoError : uint8_t {
    kConnectionRefused,
    kDisconnected,
    kTimeout,
    kSendFailed,
    kMalformedPacket,
    kBindFailed,
    kListenFailed,
};

} // namespace iouring::net
