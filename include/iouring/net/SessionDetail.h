#pragma once

#include <iouring/core/SendBuffer.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <sys/uio.h>

namespace iouring::net {

namespace buffer = iouring::core::buffer;

namespace detail {

void AdvanceSendState(std::vector<struct iovec>& iovs,
                      std::vector<buffer::SendBufferRef>& bufs,
                      std::size_t advanced);

bool IsExpectedDisconnectResult(std::int32_t result);

std::string_view DisconnectReasonForResult(std::int32_t result);

} // namespace detail

} // namespace iouring::net
