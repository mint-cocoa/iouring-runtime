#pragma once

#include "ProxyCommon.h"
#include "ProxySessions.h"

#include <iouring_runtime/core/EventHandler.h>
#include <iouring_runtime/core/SocketHandle.h>

#include <expected>
#include <memory>

namespace iouring_runtime::proxy::detail {

class ProxyConnector final : public core::ring::EventHandler {
public:
    using FinishedCallback = std::move_only_function<void(ProxyConnector*)>;

    ProxyConnector(core::ring::IoRing& ring,
                   core::buffer::BufferPool& pool,
                   const TcpProxyResolvedEndpoint& endpoint,
                   const TcpProxyConfig& config,
                   TcpProxyWorker& worker,
                   std::shared_ptr<ProxyBridge> bridge);

    void SetFinishedCallback(FinishedCallback cb);

    std::expected<void, core::io::IoError> Start();
    void Cancel();

protected:
    void OnConnect(core::ring::ConnectEvent&, std::int32_t result) final;
    void OnTimeout(core::ring::TimeoutEvent&, std::int32_t result) final;

private:
    static int CreateConnectSocket(const TcpProxyResolvedEndpoint& endpoint);
    void NotifyFinished();
    void MaybeRelease();

    core::ring::IoRing& ring_;
    core::buffer::BufferPool& pool_;
    TcpProxyResolvedEndpoint endpoint_;
    const TcpProxyConfig& config_;
    TcpProxyWorker& worker_;
    std::shared_ptr<ProxyBridge> bridge_;
    core::io::SocketHandle socket_;
    core::ring::ConnectEvent connect_ev_;
    core::ring::TimeoutEvent timeout_ev_;
    FinishedCallback on_finished_;
    std::shared_ptr<ProxyConnector> self_ref_;
    int pending_ops_{0};
    bool connect_pending_{false};
    bool timeout_armed_{false};
    bool finished_{false};
    bool finished_notified_{false};
};

} // namespace iouring_runtime::proxy::detail
