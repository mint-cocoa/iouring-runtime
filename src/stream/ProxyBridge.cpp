#include "ProxySessions.h"

#include <iouring/observability/Logging.h>

#include <cstring>
#include <utility>
#include <vector>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kProxy;
}

namespace iouring::stream::detail {

ProxyBridge::ProxyBridge(std::uint32_t pending_connect_buffer_limit)
    : pending_connect_buffer_limit_(pending_connect_buffer_limit) {}

void ProxyBridge::AttachDownstream(const std::shared_ptr<ProxyPeer>& peer) {
    if (closed_) {
        if (peer && !peer->DisconnectingPeer()) {
            peer->DisconnectPeer();
        }
        return;
    }
    downstream_ = peer;
}

void ProxyBridge::AttachUpstream(const std::shared_ptr<ProxyPeer>& peer) {
    if (closed_) {
        if (peer && !peer->DisconnectingPeer()) {
            peer->DisconnectPeer();
        }
        return;
    }
    upstream_ = peer;
    FlushPendingToUpstream();
}

bool ProxyBridge::Closed() const noexcept {
    return closed_;
}

void ProxyBridge::Forward(PeerRole from, std::span<const std::byte> data) {
    if (closed_ || data.empty()) {
        return;
    }

    if (from == PeerRole::kDownstream) {
        auto upstream = upstream_.lock();
        if (!upstream) {
            QueuePendingToUpstream(data);
            return;
        }
        if (!upstream->SendProxyPayload(data)) {
            ClosePair();
        }
        return;
    }

    auto downstream = downstream_.lock();
    if (!downstream) {
        ClosePair();
        return;
    }
    if (!downstream->SendProxyPayload(data)) {
        ClosePair();
    }
}

void ProxyBridge::OnPeerBackpressure(PeerRole role, bool active) {
    if (closed_) {
        return;
    }

    auto peer_to_throttle =
        role == PeerRole::kDownstream ? upstream_.lock() : downstream_.lock();
    if (!peer_to_throttle || peer_to_throttle->DisconnectingPeer()) {
        return;
    }

    if (active) {
        peer_to_throttle->PausePeerRecv();
    } else {
        peer_to_throttle->ResumePeerRecv();
    }
}

void ProxyBridge::OnPeerDisconnected(PeerRole role) {
    if (closed_) {
        return;
    }

    closed_ = true;
    pending_to_upstream_.clear();
    pending_to_upstream_bytes_ = 0;

    if (role == PeerRole::kUpstream) {
        if (auto downstream = downstream_.lock();
            downstream && !downstream->DisconnectingPeer()) {
            downstream->DisconnectPeerAfterFlush();
        }
        return;
    }

    if (auto upstream = upstream_.lock();
        upstream && !upstream->DisconnectingPeer()) {
        upstream->DisconnectPeer();
    }
}

void ProxyBridge::ClosePair() {
    if (closed_) {
        return;
    }
    closed_ = true;
    pending_to_upstream_.clear();
    pending_to_upstream_bytes_ = 0;

    if (auto downstream = downstream_.lock();
        downstream && !downstream->DisconnectingPeer()) {
        downstream->DisconnectPeer();
    }
    if (auto upstream = upstream_.lock();
        upstream && !upstream->DisconnectingPeer()) {
        upstream->DisconnectPeer();
    }
}

void ProxyBridge::QueuePendingToUpstream(std::span<const std::byte> data) {
    const auto next_size = pending_to_upstream_bytes_ + data.size();
    if (pending_connect_buffer_limit_ != 0 &&
        next_size > pending_connect_buffer_limit_) {
        obs::LogWarn(kLogCategory,
            "TcpProxyServer: pending upstream buffer limit {} bytes exceeded",
            pending_connect_buffer_limit_);
        ClosePair();
        return;
    }

    std::vector<std::byte> chunk(data.size());
    std::memcpy(chunk.data(), data.data(), data.size());
    pending_to_upstream_bytes_ = next_size;
    pending_to_upstream_.push_back(std::move(chunk));
}

void ProxyBridge::FlushPendingToUpstream() {
    auto upstream = upstream_.lock();
    if (!upstream) {
        return;
    }

    auto pending = std::move(pending_to_upstream_);
    pending_to_upstream_bytes_ = 0;
    for (auto& chunk : pending) {
        if (!upstream->SendProxyPayload(
                std::span<const std::byte>(chunk.data(), chunk.size()))) {
            ClosePair();
            return;
        }
    }
}

} // namespace iouring::stream::detail
