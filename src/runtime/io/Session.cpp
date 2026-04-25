#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/observability/Profiler.h>

#include <liburing.h>
#include <iouring_runtime/observability/Logging.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring_runtime::core::io {

Session::Session(int fd, IoRing& ring, BufferPool& pool,
                 std::uint32_t send_queue_max_pending)
    : socket_(fd)
    , ring_(ring)
    , pool_(pool)
    , send_queue_(send_queue_max_pending) {}

Session::~Session() = default;

std::string Session::FormatSockAddr(const struct sockaddr* sa, socklen_t len) {
    if (!sa || len == 0) return {};

    char host[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;

    if (sa->sa_family == AF_INET) {
        if (len < static_cast<socklen_t>(sizeof(sockaddr_in))) return {};
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        if (!::inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host))) return {};
        port = ntohs(in->sin_port);
        return std::string(host) + ":" + std::to_string(port);
    }
    if (sa->sa_family == AF_INET6) {
        if (len < static_cast<socklen_t>(sizeof(sockaddr_in6))) return {};
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        if (!::inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host))) return {};
        port = ntohs(in6->sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }
    // AF_UNIX and anything else: no IP:port representation. Empty string
    // signals "not a network peer" to handlers.
    return {};
}

std::string_view Session::RemoteAddr() {
    if (!remote_addr_resolved_) {
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        if (::getpeername(socket_.Get(),
                          reinterpret_cast<sockaddr*>(&ss), &len) == 0) {
            remote_addr_ = FormatSockAddr(reinterpret_cast<sockaddr*>(&ss), len);
        }
        remote_addr_resolved_ = true;
    }
    return remote_addr_;
}

void Session::SetInactivityTimeout(std::chrono::milliseconds timeout) {
    inactivity_timeout_ = timeout;
    if (timeout.count() == 0) return;

    // Tick at a quarter of the deadline so a stalled session is detected
    // within 1.25x the configured bound. Minimum 100ms to avoid runaway
    // CQE volume on very short deadlines.
    auto interval = timeout / 4;
    SetTimeoutCheckInterval(interval);
}

void Session::SetTimeoutCheckInterval(std::chrono::milliseconds interval) {
    if (interval.count() == 0) return;
    if (interval < std::chrono::milliseconds{100})
        interval = std::chrono::milliseconds{100};
    if (check_interval_.count() == 0 || interval < check_interval_)
        check_interval_ = interval;
}

void Session::Start() {
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    self_ref_ = self;  // self-ownership: Session keeps itself alive
    recv_ev_.SetOwner(self);
    send_ev_.SetOwner(self);
    disconnect_ev_.SetOwner(self);
    timeout_ev_.SetOwner(self);
    last_activity_ = std::chrono::steady_clock::now();
    OnConnected();
    if (on_connected_)
        on_connected_(self);
    RegisterRecv();
    ArmWatchdog();
}

void Session::PauseRecv() {
    if (disconnecting_ || recv_paused_) {
        return;
    }

    recv_paused_ = true;
    if (recv_armed_ && !recv_cancel_requested_) {
        if (ring_.PrepCancel(recv_ev_)) {
            recv_cancel_requested_ = true;
            ring_.Submit();
        }
    }
}

void Session::ResumeRecv() {
    if (!recv_paused_) {
        return;
    }

    recv_paused_ = false;
    if (!recv_armed_ && !recv_cancel_requested_ && !disconnecting_) {
        RegisterRecv();
    }
}

void Session::DisconnectAfterFlush() {
    if (disconnecting_) return;

    disconnect_after_flush_ = true;
    MaybeDisconnectAfterFlush();
}

void Session::Disconnect() {
    if (disconnecting_) return;
    disconnecting_ = true;
    obs::LogWarn(kLogCategory, "Session[fd={} sid={}]: [DISC:ENTER] Disconnect() called", Fd(), session_id_);

    // 1. Shutdown the socket — causes recv to EOF and send to error,
    //    which naturally terminates the multishot recv.
    if (!ring_.PrepDisconnect(disconnect_ev_, Fd())) {
        // SQE full: cannot submit shutdown. Force-release to avoid leak.
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SHUTDOWN] PrepDisconnect failed", Fd());
        pending_io_ = 0;
        TryRelease();
        return;
    }
    ++pending_io_;  // disconnect CQE pending

    // 2. Cancel multishot recv for faster cleanup (best-effort).
    //    If PrepCancel fails (SQE full), shutdown alone will still
    //    terminate the multishot by delivering EOF.
    ring_.PrepCancel(recv_ev_);

    // 3. Cancel inactivity watchdog if one is in flight — otherwise we'd
    //    wait up to check_interval_ before pending_io_ reaches zero.
    if (timeout_armed_) {
        ring_.PrepCancel(timeout_ev_);
    }

    ring_.Submit();
}

// -- Recv handling (fast/slow path + multishot + ENOBUFS) ----

void Session::OnRecv(ring::RecvEvent& ev,
                     std::int32_t res, std::uint32_t flags) {
    const bool more = (flags & IORING_CQE_F_MORE) != 0;
    const bool has_buffer = (flags & IORING_CQE_F_BUFFER) != 0;
    const bool was_cancel_requested = recv_cancel_requested_;

    // No F_MORE means the kernel will send no more CQEs for this SQE.
    if (!more) {
        recv_armed_ = false;
        recv_cancel_requested_ = false;
        --pending_io_;
    }

    // Always return provided buffer to the ring, even during disconnect.
    // Failing to return leaks buffers from the provided buffer pool.
    if (has_buffer) {
        std::uint16_t buf_id = flags >> IORING_CQE_BUFFER_SHIFT;
        auto& buf_ring = ring_.BufRing();

        if (res > 0 && !disconnecting_) {
            last_activity_ = std::chrono::steady_clock::now();
            auto view = buf_ring.View(buf_id, static_cast<std::uint32_t>(res));
            OnRecv(view);
        }

        buf_ring.Return(buf_id);
    }

    // During disconnect, skip normal error handling — just wait for
    // all in-flight ops to settle before releasing self_ref_.
    if (disconnecting_) {
        TryRelease();
        return;
    }

    if (was_cancel_requested && res == -ECANCELED) {
        if (!recv_paused_) {
            RegisterRecv();
        }
        return;
    }

    // Normal error handling
    if (res == 0) {
        // Peer closed session
        obs::LogWarn(kLogCategory, "Session[fd={}]: [DISC:PEER_CLOSE] peer closed connection", Fd());
        Disconnect();
        return;
    }

    if (res == -ENOBUFS) {
        // Buffer pool exhausted — multishot terminated, re-register
        obs::LogWarn(kLogCategory, "Session[fd={}]: [WARN:ENOBUFS] provided buffer pool exhausted, re-registering", Fd());
        if (!recv_paused_) {
            RegisterRecv();
        }
        return;
    }

    if (res < 0) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:RECV_ERR] recv error res={}", Fd(), res);
        Disconnect();
        return;
    }

    // Multishot ended normally (e.g. internal resource limit) — re-register
    if (!more && !recv_paused_)
        RegisterRecv();
}

void Session::OnDisconnect(ring::DisconnectEvent& ev,
                           std::int32_t res) {
    --pending_io_;
    TryRelease();
}

// -- Self-ownership release gate ----

void Session::TryRelease() {
    if (pending_io_ > 0)
        return;

    obs::LogWarn(kLogCategory, "Session[fd={} sid={}]: [DISC:RELEASED] session destroyed", Fd(), session_id_);
    OnDisconnected();
    if (on_disconnect_)
        on_disconnect_(std::static_pointer_cast<Session>(shared_from_this()));

    // Release self-ownership. The Dispatch keep_alive vector still holds a
    // reference, so actual destruction is deferred until the CQE batch ends.
    self_ref_.reset();
}

void Session::ReleaseOwnership() {
    self_ref_.reset();
}

bool Session::CanDisconnectAfterFlush() const {
    return disconnect_after_flush_ &&
           send_queue_.Snapshot().current_depth == 0 &&
           in_flight_bufs_.empty() &&
           !HasPendingAppWork();
}

void Session::MaybeDisconnectAfterFlush() {
    if (disconnecting_) return;
    if (CanDisconnectAfterFlush()) {
        Disconnect();
    }
}

bool Session::HasPendingSocketWrites() const {
    return send_queue_.Snapshot().current_depth != 0 || !in_flight_bufs_.empty();
}

void Session::BeginAppIo() {
    ++pending_io_;
}

void Session::EndAppIo() {
    --pending_io_;
    if (disconnecting_) {
        TryRelease();
        return;
    }
    MaybeDisconnectAfterFlush();
}

// -- SQE registration ----

void Session::RegisterRecv() {
    if (disconnecting_) return;
    if (recv_paused_ || recv_armed_) return;
    ++pending_io_;
    if (!ring_.PrepRecvMultishot(recv_ev_, Fd())) {
        --pending_io_;
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_RECV] PrepRecvMultishot failed", Fd());
        Disconnect();
        return;
    }
    recv_armed_ = true;
    ring_.Submit();
}

void Session::ArmWatchdog() {
    if (check_interval_.count() == 0) return;  // disabled
    if (timeout_armed_) return;
    if (disconnecting_) return;

    ++pending_io_;
    if (!ring_.PrepTimeout(timeout_ev_, check_interval_)) {
        --pending_io_;
        obs::LogError(kLogCategory, "Session[fd={}]: ArmWatchdog SQE full", Fd());
        return;
    }
    timeout_armed_ = true;
    ring_.Submit();
}

void Session::OnTimeout(ring::TimeoutEvent& /*ev*/, std::int32_t res) {
    --pending_io_;
    timeout_armed_ = false;

    if (disconnecting_) {
        // -ECANCELED from Disconnect's cancel SQE, or the watchdog just
        // happened to fire — either way, drain the pending-io count.
        TryRelease();
        return;
    }

    // -ECANCELED without disconnect shouldn't normally happen — the only
    // cancel site is Disconnect(). Treat it as a no-op.
    if (res == -ECANCELED) return;

    auto now = std::chrono::steady_clock::now();
    if (OnTimeoutTick(now)) return;

    auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_activity_);
    if (inactivity_timeout_.count() > 0 && idle >= inactivity_timeout_) {
        obs::LogWarn(kLogCategory, "Session[fd={}]: [DISC:INACTIVITY] idle={}ms timeout={}ms",
                     Fd(), idle.count(), inactivity_timeout_.count());
        Disconnect();
        return;
    }

    // Still within budget — rearm for the next check tick.
    ArmWatchdog();
}

} // namespace iouring_runtime::core::io
