#pragma once

#include "DownstreamTlsContext.h"
#include "ProxyCommon.h"

#include <memory>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace iouring_runtime::proxy::detail {

enum class PeerRole {
    kDownstream,
    kUpstream,
};

using DownstreamTlsReadyCallback = std::move_only_function<bool(std::string_view)>;

class ProxyPeer {
public:
    virtual ~ProxyPeer() = default;

    virtual bool SendProxyPayload(std::span<const std::byte> data) = 0;
    virtual void DisconnectPeer() = 0;
    virtual void DisconnectPeerAfterFlush() = 0;
    virtual void PausePeerRecv() = 0;
    virtual void ResumePeerRecv() = 0;
    virtual bool DisconnectingPeer() const = 0;
};

class ProxyBridge {
public:
    explicit ProxyBridge(std::uint32_t pending_connect_buffer_limit);

    void AttachDownstream(const std::shared_ptr<ProxyPeer>& peer);
    void AttachUpstream(const std::shared_ptr<ProxyPeer>& peer);
    bool Closed() const noexcept;
    void Forward(PeerRole from, std::span<const std::byte> data);
    void OnPeerBackpressure(PeerRole role, bool active);
    void OnPeerDisconnected(PeerRole role);
    void ClosePair();

private:
    void QueuePendingToUpstream(std::span<const std::byte> data);
    void FlushPendingToUpstream();

    std::weak_ptr<ProxyPeer> downstream_;
    std::weak_ptr<ProxyPeer> upstream_;
    std::vector<std::vector<std::byte>> pending_to_upstream_;
    std::size_t pending_to_upstream_bytes_{0};
    std::size_t pending_connect_buffer_limit_{0};
    bool closed_{false};
};

core::io::SessionRef CreatePlainProxySession(
    int fd, core::ring::IoRing& ring, core::buffer::BufferPool& pool,
    std::shared_ptr<ProxyBridge> bridge, PeerRole role,
    std::uint32_t send_queue_max_pending);

core::io::SessionRef CreateTlsProxySession(
    int fd, core::ring::IoRing& ring, core::buffer::BufferPool& pool,
    std::shared_ptr<ProxyBridge> bridge,
    std::shared_ptr<DownstreamTlsContext> tls_context,
    DownstreamTlsReadyCallback on_ready,
    PeerRole role, std::uint32_t send_queue_max_pending,
    std::uint32_t pending_plaintext_limit);

std::shared_ptr<ProxyPeer> ToProxyPeer(const core::io::SessionRef& session);

} // namespace iouring_runtime::proxy::detail
