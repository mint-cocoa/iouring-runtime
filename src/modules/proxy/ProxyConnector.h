#pragma once

#include "ProxyCommon.h"
#include "ProxySessions.h"

#include <iouring_runtime/core/EventHandler.h>
#include <iouring_runtime/core/SocketHandle.h>

#include <expected>
#include <functional>
#include <memory>

namespace iouring_runtime::proxy::detail {

class ProxyConnector final : public core::ring::EventHandler {
public:
    using FinishedCallback = std::move_only_function<void(ProxyConnector*)>;
    using ConnectResultCallback =
        std::move_only_function<void(bool success, bool timeout)>;

    ProxyConnector(core::ring::IoRing& ring,
                   core::buffer::BufferPool& pool,
                   const TcpProxyResolvedEndpoint& endpoint,
                   const TcpProxyConfig& config,
                   TcpProxyWorker& worker,
                   std::shared_ptr<ProxyBridge> bridge);

    void SetFinishedCallback(FinishedCallback cb);
    void SetConnectResultCallback(ConnectResultCallback cb);

    std::expected<void, core::io::IoError> Start();
    void Cancel();

protected:
    void OnConnect(core::ring::ConnectEvent&, std::int32_t result) final;
    void OnTimeout(core::ring::TimeoutEvent&, std::int32_t result) final;
    void OnCancel(core::ring::CancelEvent&, std::int32_t result) final;

private:
    static int CreateConnectSocket(const TcpProxyResolvedEndpoint& endpoint);
    void NotifyConnectResult(bool success, bool timeout);
    void NotifyFinished();
    void MaybeRelease();

    core::ring::IoRing& ring_;
    core::buffer::BufferPool& pool_;
    TcpProxyResolvedEndpoint endpoint_;
    const TcpProxyConfig& config_;
    TcpProxyWorker& worker_;
    std::shared_ptr<ProxyBridge> bridge_;
    core::io::SocketHandle socket_;
    core::ring::ConnectEvent* active_connect_ev_{nullptr};
    core::ring::TimeoutEvent* active_timeout_ev_{nullptr};
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

} // namespace iouring_runtime::proxy::detail
