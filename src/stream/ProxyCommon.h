#pragma once

#include <iouring/core/SendBuffer.h>
#include <iouring/net/Session.h>
#include <iouring/event/Worker.h>
#include <iouring/stream/TcpProxyConfig.h>

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iouring::stream::detail {

class ProxyConnector;
struct DownstreamTlsContext;

struct TcpProxyResolvedEndpoint {
    sockaddr_storage storage{};
    socklen_t len{0};
    int family{AF_UNSPEC};
    std::string display;
};

struct TcpProxyResolvedRoute {
    std::string hostname;
    TcpProxyResolvedEndpoint endpoint;
};

struct TcpProxyWorker {
    TcpProxyWorker();

    std::uint16_t index{0};
    int pinned_cpu{-1};
    std::atomic<std::size_t> live_connectors{0};
    std::mutex connectors_mu;
    std::unordered_map<ProxyConnector*, std::shared_ptr<ProxyConnector>> connectors;
    std::atomic<core::SessionId> next_session_id{1};
    std::unique_ptr<event::Worker> io_worker;
};

void ConfigureProxySession(const net::SessionRef& session,
                           TcpProxyWorker& worker,
                           const TcpProxyConfig& config);

std::expected<core::buffer::SendBufferRef, core::CoreError> CopyToSendBuffer(
    core::buffer::BufferPool& pool, std::span<const std::byte> data);
std::expected<core::buffer::SendBufferRef, core::CoreError> CopyToSendBuffer(
    core::buffer::BufferPool& pool, std::string_view data);

} // namespace iouring::stream::detail
