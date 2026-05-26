#pragma once

#include <iouring_runtime/core/SendBuffer.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <sys/uio.h>

namespace iouring_runtime::core::io::detail {

inline void AdvanceSendState(std::vector<struct iovec>& iovs,
                             std::vector<buffer::SendBufferRef>& bufs,
                             std::size_t advanced) {
    std::size_t consumed = 0;
    while (consumed < iovs.size() && advanced >= iovs[consumed].iov_len) {
        advanced -= iovs[consumed].iov_len;
        ++consumed;
    }
    if (consumed > 0) {
        iovs.erase(iovs.begin(), iovs.begin() + consumed);
        bufs.erase(bufs.begin(), bufs.begin() + consumed);
    }
    if (advanced > 0 && !iovs.empty()) {
        auto& front = iovs.front();
        front.iov_base = static_cast<std::byte*>(front.iov_base) + advanced;
        front.iov_len -= advanced;
    }
}

inline bool IsExpectedDisconnectResult(std::int32_t result) {
    if (result == 0) {
        return true;
    }
    if (result > 0) {
        return false;
    }

    switch (-result) {
    case ECONNRESET:
    case EPIPE:
    case ENOTCONN:
    case ESHUTDOWN:
        return true;
    default:
        return false;
    }
}

inline std::string_view DisconnectReasonForResult(std::int32_t result) {
    if (result == 0) {
        return "PEER_CLOSE";
    }
    if (result > 0) {
        return "OK";
    }

    switch (-result) {
    case ECONNRESET:
        return "CONNECTION_RESET";
    case EPIPE:
        return "BROKEN_PIPE";
    case ENOTCONN:
        return "NOT_CONNECTED";
    case ESHUTDOWN:
        return "SOCKET_SHUTDOWN";
    default:
        return "TRANSPORT_ERROR";
    }
}

} // namespace iouring_runtime::core::io::detail
