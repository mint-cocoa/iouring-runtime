#include "ProxyCommon.h"

#include <iouring/event/IoRing.h>
#include <iouring/net/SessionControl.h>

#include <cstring>

namespace iouring::stream::detail {

TcpProxyWorker::TcpProxyWorker()
    = default;

void ConfigureProxySession(const net::SessionRef& session,
                           TcpProxyWorker& worker,
                           const TcpProxyConfig& config) {
    net::SessionControl::SetSessionId(
        *session,
        worker.next_session_id.fetch_add(1, std::memory_order_relaxed));
    net::SessionControl::SetInactivityTimeout(
        *session, config.timeouts.inactivity);
    net::SessionControl::SetBackpressureWatermarks(
        *session,
        config.backpressure.send_queue_high_watermark,
        config.backpressure.send_queue_low_watermark);
    net::SessionControl::SetBackpressureByteWatermarks(
        *session,
        config.backpressure.send_queue_high_bytes,
        config.backpressure.send_queue_low_bytes);
    net::SessionControl::SetPauseRecvOnBackpressure(
        *session,
        config.backpressure.pause_recv_on_high_watermark);
    net::SessionControl::SetPausedRecvByteLimit(
        *session,
        config.backpressure.paused_recv_byte_limit);
    net::SessionControl::SetBackpressureDisconnectDelay(
        *session,
        config.backpressure.disconnect_after);
    net::SessionControl::SetDisconnectOnHighWatermark(
        *session,
        config.backpressure.disconnect_on_high_watermark);
}

std::expected<core::buffer::SendBufferRef, core::CoreError> CopyToSendBuffer(
    core::buffer::BufferPool& pool, std::span<const std::byte> data) {
    auto buffer_result =
        pool.Allocate(static_cast<std::uint32_t>(data.size()));
    if (!buffer_result) {
        return std::unexpected(buffer_result.error());
    }

    auto buffer = std::move(*buffer_result);
    std::memcpy(buffer->Writable().data(), data.data(), data.size());
    buffer->Commit(static_cast<std::uint32_t>(data.size()));
    return buffer;
}

std::expected<core::buffer::SendBufferRef, core::CoreError> CopyToSendBuffer(
    core::buffer::BufferPool& pool, std::string_view data) {
    return CopyToSendBuffer(
        pool,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

} // namespace iouring::stream::detail
