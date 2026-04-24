#include "ProxyCommon.h"

#include <iouring_runtime/core/IoRing.h>

#include <cstring>

namespace iouring_runtime::proxy::detail {

TcpProxyWorker::TcpProxyWorker()
    : pool(256 * 1024, 1024) {}

void ConfigureProxySession(const core::io::SessionRef& session,
                           TcpProxyWorker& worker,
                           const TcpProxyConfig& config) {
    auto* raw_worker = &worker;
    session->SetSessionId(raw_worker->next_session_id.fetch_add(
        1, std::memory_order_relaxed));
    session->SetInactivityTimeout(config.timeouts.inactivity);
    session->SetBackpressureWatermarks(
        config.backpressure.send_queue_high_watermark,
        config.backpressure.send_queue_low_watermark);
    session->SetDisconnectOnHighWatermark(
        config.backpressure.disconnect_on_high_watermark);
    session->SetConnectedCallback([raw_worker](core::io::SessionRef session_ref) {
        raw_worker->live_sessions.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(raw_worker->sessions_mu);
        raw_worker->sessions.emplace(session_ref.get(), std::move(session_ref));
    });
    session->SetDisconnectCallback(
        [raw_worker](core::io::SessionRef session_ref) {
            raw_worker->live_sessions.fetch_sub(1, std::memory_order_relaxed);
            std::lock_guard lock(raw_worker->sessions_mu);
            raw_worker->sessions.erase(session_ref.get());
        });
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
