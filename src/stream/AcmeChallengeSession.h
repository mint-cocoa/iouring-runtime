#pragma once

#include <iouring/event/IoRing.h>
#include <iouring/core/SendBuffer.h>
#include <iouring/net/Session.h>

#include <memory>
#include <string>

namespace iouring::stream::detail {

net::SessionRef CreateAcmeChallengeSession(
    int fd, event::IoRing& ring, core::buffer::BufferPool& pool,
    std::string challenge_webroot, std::uint32_t send_queue_max_pending);

} // namespace iouring::stream::detail
