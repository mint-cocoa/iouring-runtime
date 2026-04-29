#include "ProxyConnector.h"

#include <iouring_runtime/core/IoRing.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace iouring_runtime::proxy::detail {

ProxyConnector::ProxyConnector(core::ring::IoRing& ring,
                               core::buffer::BufferPool& pool,
                               const TcpProxyResolvedEndpoint& endpoint,
                               const TcpProxyConfig& config,
                               TcpProxyWorker& worker,
                               std::shared_ptr<ProxyBridge> bridge)
    : ring_(ring)
    , pool_(pool)
    , endpoint_(endpoint)
    , config_(config)
    , worker_(worker)
    , bridge_(std::move(bridge)) {}

void ProxyConnector::SetFinishedCallback(FinishedCallback cb) {
    on_finished_ = std::move(cb);
}

std::expected<void, core::io::IoError> ProxyConnector::Start() {
    auto self = std::static_pointer_cast<ProxyConnector>(shared_from_this());
    self_ref_ = self;
    connect_ev_.SetOwner(self);
    timeout_ev_.SetOwner(self);

    const int fd = CreateConnectSocket(endpoint_);
    if (fd < 0) {
        self_ref_.reset();
        return std::unexpected(core::io::IoError::kConnectionRefused);
    }
    socket_.Reset(fd);

    ++pending_ops_;
    connect_pending_ = true;
    if (!ring_.PrepConnect(connect_ev_, socket_.Get(),
                           reinterpret_cast<const sockaddr*>(&endpoint_.storage),
                           endpoint_.len)) {
        --pending_ops_;
        connect_pending_ = false;
        socket_.Reset();
        self_ref_.reset();
        return std::unexpected(core::io::IoError::kConnectionRefused);
    }

    if (config_.timeouts.connect.count() > 0) {
        if (ring_.PrepTimeout(timeout_ev_, config_.timeouts.connect)) {
            ++pending_ops_;
            timeout_armed_ = true;
        }
    }

    ring_.Submit();
    return {};
}

void ProxyConnector::Cancel() {
    if (finished_) {
        return;
    }

    finished_ = true;
    bridge_->ClosePair();
    socket_.Reset();

    if (connect_pending_) {
        ring_.PrepCancel(connect_ev_);
    }
    if (timeout_armed_) {
        ring_.PrepCancel(timeout_ev_);
        timeout_armed_ = false;
    }
    ring_.Submit();

    NotifyFinished();
    MaybeRelease();
}

void ProxyConnector::OnConnect(core::ring::ConnectEvent&, std::int32_t result) {
    --pending_ops_;
    connect_pending_ = false;

    if (finished_) {
        MaybeRelease();
        return;
    }

    if (timeout_armed_) {
        ring_.PrepCancel(timeout_ev_);
        ring_.Submit();
        timeout_armed_ = false;
    }

    if (result < 0 || bridge_->Closed()) {
        finished_ = true;
        socket_.Reset();
        bridge_->ClosePair();
        NotifyFinished();
        MaybeRelease();
        return;
    }

    auto upstream = CreatePlainProxySession(
        socket_.Release(), ring_, pool_, bridge_, PeerRole::kUpstream,
        config_.backpressure.send_queue_max_pending);
    ConfigureProxySession(upstream, worker_, config_);
    if (worker_.io_worker) {
        worker_.io_worker->TrackSession(upstream);
    }
    upstream->Start();
    bridge_->AttachUpstream(ToProxyPeer(upstream));

    finished_ = true;
    NotifyFinished();
    MaybeRelease();
}

void ProxyConnector::OnTimeout(core::ring::TimeoutEvent&, std::int32_t result) {
    --pending_ops_;

    if (finished_) {
        MaybeRelease();
        return;
    }

    if (result == -ECANCELED) {
        MaybeRelease();
        return;
    }

    finished_ = true;
    bridge_->ClosePair();
    socket_.Reset();

    if (connect_pending_) {
        ring_.PrepCancel(connect_ev_);
        ring_.Submit();
    }

    NotifyFinished();
    MaybeRelease();
}

int ProxyConnector::CreateConnectSocket(const TcpProxyResolvedEndpoint& endpoint) {
    int fd = ::socket(endpoint.family,
                      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    return fd;
}

void ProxyConnector::NotifyFinished() {
    if (finished_notified_) {
        return;
    }
    finished_notified_ = true;
    if (on_finished_) {
        on_finished_(this);
    }
}

void ProxyConnector::MaybeRelease() {
    if (pending_ops_ == 0) {
        self_ref_.reset();
    }
}

} // namespace iouring_runtime::proxy::detail
