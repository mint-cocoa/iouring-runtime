#pragma once

#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Session.h>

#include <memory>
#include <string>

namespace iouring_runtime::proxy::detail {

core::io::SessionRef CreateAcmeChallengeSession(
    int fd, core::ring::IoRing& ring, core::buffer::BufferPool& pool,
    std::string challenge_webroot, std::uint32_t send_queue_max_pending);

} // namespace iouring_runtime::proxy::detail
