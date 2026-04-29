#include "ProxyCommon.h"

#include <iouring_runtime/core/IoRing.h>

#include <cstring>

namespace iouring_runtime::proxy::detail {

TcpProxyWorker::TcpProxyWorker()
    = default;

void ConfigureProxySession(const core::io::SessionRef& session,
                           TcpProxyWorker& worker,
                           const TcpProxyConfig& config) {
    session->SetSessionId(worker.next_session_id.fetch_add(
        1, std::memory_order_relaxed));
    session->SetInactivityTimeout(config.timeouts.inactivity);
    session->SetBackpressureWatermarks(
        config.backpressure.send_queue_high_watermark,
        config.backpressure.send_queue_low_watermark);
    session->SetDisconnectOnHighWatermark(
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

} // namespace iouring_runtime::proxy::detail
