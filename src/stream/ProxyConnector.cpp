#include "ProxyConnector.h"

#include <iouring/event/IoRing.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <new>

namespace iouring::stream::detail {

ProxyConnector::ProxyConnector(event::IoRing& ring,
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

void ProxyConnector::SetConnectResultCallback(ConnectResultCallback cb) {
    on_connect_result_ = std::move(cb);
}

std::expected<void, net::IoError> ProxyConnector::Start() {
    auto self = shared_from_this();
    self_ref_ = self;

    const int fd = CreateConnectSocket(endpoint_);
    if (fd < 0) {
        self_ref_.reset();
        return std::unexpected(net::IoError::kConnectionRefused);
    }
    socket_.Reset(fd);

    auto* connect_ev = new (std::nothrow) event::ConnectEvent();
    if (!connect_ev) {
        socket_.Reset();
        self_ref_.reset();
        return std::unexpected(net::IoError::kConnectionRefused);
    }
    event::BindCompletion(*connect_ev, self, &ProxyConnector::OnConnect);
    connect_ev->SetAutoDelete(true);

    ++pending_ops_;
    connect_pending_ = true;
    if (!ring_.PrepConnect(*connect_ev, socket_.Get(),
                           reinterpret_cast<const sockaddr*>(&endpoint_.storage),
                           endpoint_.len)) {
        --pending_ops_;
        connect_pending_ = false;
        delete connect_ev;
        socket_.Reset();
        self_ref_.reset();
        return std::unexpected(net::IoError::kConnectionRefused);
    }
    active_connect_ev_ = connect_ev;

    if (config_.timeouts.connect.count() > 0) {
        auto* timeout_ev = new (std::nothrow) event::TimeoutEvent();
        if (timeout_ev) {
            event::BindCompletion(*timeout_ev, self, &ProxyConnector::OnTimeout);
            timeout_ev->SetAutoDelete(true);
        }
        if (timeout_ev && ring_.PrepTimeout(*timeout_ev, config_.timeouts.connect)) {
            ++pending_ops_;
            timeout_armed_ = true;
            active_timeout_ev_ = timeout_ev;
        } else {
            delete timeout_ev;
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

    if (connect_pending_ && active_connect_ev_) {
        auto self = shared_from_this();
        auto* cancel_ev = new (std::nothrow) event::CancelEvent(active_connect_ev_);
        if (cancel_ev) {
            event::BindCompletion(*cancel_ev, self, &ProxyConnector::OnCancel);
            cancel_ev->SetAutoDelete(true);
            if (ring_.PrepCancel(*active_connect_ev_, cancel_ev)) {
                ++pending_ops_;
            } else {
                delete cancel_ev;
            }
        }
    }
    if (timeout_armed_ && active_timeout_ev_) {
        auto self = shared_from_this();
        auto* cancel_ev = new (std::nothrow) event::CancelEvent(active_timeout_ev_);
        if (cancel_ev) {
            event::BindCompletion(*cancel_ev, self, &ProxyConnector::OnCancel);
            cancel_ev->SetAutoDelete(true);
            if (ring_.PrepCancel(*active_timeout_ev_, cancel_ev)) {
                ++pending_ops_;
            } else {
                delete cancel_ev;
            }
        }
        timeout_armed_ = false;
    }
    ring_.Submit();

    NotifyFinished();
    MaybeRelease();
}

event::DispatchResult ProxyConnector::OnConnect(event::ConnectEvent& ev, std::int32_t result) {
    --pending_ops_;
    if (&ev == active_connect_ev_) {
        active_connect_ev_ = nullptr;
    }
    connect_pending_ = false;

    if (finished_) {
        MaybeRelease();
        return event::DispatchResult::kComplete;
    }

    if (timeout_armed_ && active_timeout_ev_) {
        auto self = shared_from_this();
        auto* cancel_ev = new (std::nothrow) event::CancelEvent(active_timeout_ev_);
        if (cancel_ev) {
            event::BindCompletion(*cancel_ev, self, &ProxyConnector::OnCancel);
            cancel_ev->SetAutoDelete(true);
            if (ring_.PrepCancel(*active_timeout_ev_, cancel_ev)) {
                ++pending_ops_;
                ring_.Submit();
            } else {
                delete cancel_ev;
            }
        }
        timeout_armed_ = false;
    }

    if (result < 0 || bridge_->Closed()) {
        NotifyConnectResult(false, false);
        finished_ = true;
        socket_.Reset();
        bridge_->ClosePair();
        NotifyFinished();
        MaybeRelease();
        return event::DispatchResult::kComplete;
    }

    auto upstream = CreatePlainProxySession(
        socket_.Release(), ring_, pool_, bridge_, PeerRole::kUpstream,
        config_.backpressure.send_queue_max_pending);
    ConfigureProxySession(upstream, worker_, config_);
    upstream->Start();
    bridge_->AttachUpstream(ToProxyPeer(upstream));

    NotifyConnectResult(true, false);
    finished_ = true;
    NotifyFinished();
    MaybeRelease();
    return event::DispatchResult::kComplete;
}

event::DispatchResult ProxyConnector::OnTimeout(event::TimeoutEvent& ev, std::int32_t result) {
    --pending_ops_;
    if (&ev == active_timeout_ev_) {
        active_timeout_ev_ = nullptr;
    }
    timeout_armed_ = false;

    if (finished_) {
        MaybeRelease();
        return event::DispatchResult::kComplete;
    }

    if (result == -ECANCELED) {
        MaybeRelease();
        return event::DispatchResult::kComplete;
    }

    finished_ = true;
    NotifyConnectResult(false, true);
    bridge_->ClosePair();
    socket_.Reset();

    if (connect_pending_ && active_connect_ev_) {
        auto self = shared_from_this();
        auto* cancel_ev = new (std::nothrow) event::CancelEvent(active_connect_ev_);
        if (cancel_ev) {
            event::BindCompletion(*cancel_ev, self, &ProxyConnector::OnCancel);
            cancel_ev->SetAutoDelete(true);
            if (ring_.PrepCancel(*active_connect_ev_, cancel_ev)) {
                ++pending_ops_;
                ring_.Submit();
            } else {
                delete cancel_ev;
            }
        }
    }

    NotifyFinished();
    MaybeRelease();
    return event::DispatchResult::kComplete;
}

event::DispatchResult ProxyConnector::OnCancel(event::CancelEvent&, std::int32_t) {
    --pending_ops_;
    MaybeRelease();
    return event::DispatchResult::kComplete;
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

void ProxyConnector::NotifyConnectResult(bool success, bool timeout) {
    if (connect_result_notified_) {
        return;
    }
    connect_result_notified_ = true;
    if (on_connect_result_) {
        on_connect_result_(success, timeout);
    }
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

} // namespace iouring::stream::detail
