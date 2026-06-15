#pragma once

#include "ProxyCommon.h"
#include "ProxySessions.h"

#include <iouring/event/RingEvent.h>
#include <iouring/net/SocketHandle.h>

#include <expected>
#include <functional>
#include <memory>

namespace iouring::stream::detail {

class ProxyConnector final : public std::enable_shared_from_this<ProxyConnector> {
public:
    using FinishedCallback = std::move_only_function<void(ProxyConnector*)>;
    using ConnectResultCallback =
        std::move_only_function<void(bool success, bool timeout)>;

    ProxyConnector(event::IoRing& ring,
                   core::buffer::BufferPool& pool,
                   const TcpProxyResolvedEndpoint& endpoint,
                   const TcpProxyConfig& config,
                   TcpProxyWorker& worker,
                   std::shared_ptr<ProxyBridge> bridge);

    void SetFinishedCallback(FinishedCallback cb);
    void SetConnectResultCallback(ConnectResultCallback cb);

    std::expected<void, net::IoError> Start();
    void Cancel();

private:
    event::DispatchResult OnConnect(event::ConnectEvent&, std::int32_t result);
    event::DispatchResult OnTimeout(event::TimeoutEvent&, std::int32_t result);
    event::DispatchResult OnCancel(event::CancelEvent&, std::int32_t result);
    static int CreateConnectSocket(const TcpProxyResolvedEndpoint& endpoint);
    void NotifyConnectResult(bool success, bool timeout);
    void NotifyFinished();
    void MaybeRelease();

    event::IoRing& ring_;
    core::buffer::BufferPool& pool_;
    TcpProxyResolvedEndpoint endpoint_;
    const TcpProxyConfig& config_;
    TcpProxyWorker& worker_;
    std::shared_ptr<ProxyBridge> bridge_;
    net::SocketHandle socket_;
    event::ConnectEvent* active_connect_ev_{nullptr};
    event::TimeoutEvent* active_timeout_ev_{nullptr};
    FinishedCallback on_finished_;
    ConnectResultCallback on_connect_result_;
    std::shared_ptr<ProxyConnector> self_ref_;
    int pending_ops_{0};
    bool connect_pending_{false};
    bool timeout_armed_{false};
    bool finished_{false};
    bool finished_notified_{false};
    bool connect_result_notified_{false};
};

} // namespace iouring::stream::detail
