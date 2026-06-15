#include <iouring/net/Session.h>
#include <iouring/event/IoRing.h>
#include <iouring/net/SessionDetail.h>

#include <liburing.h>
#include <iouring/observability/Logging.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring::net {

bool Session::ClosingStarted() const noexcept {
    return state_.load(std::memory_order_acquire) != SessionState::kOpen;
}

bool Session::CanUseOnOwner() const noexcept {
    return state_.load(std::memory_order_relaxed) == SessionState::kOpen;
}

bool Session::TryBeginDisconnectOnOwner() noexcept {
    auto expected = SessionState::kOpen;
    return state_.compare_exchange_strong(expected, SessionState::kClosing,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
}

void Session::MarkDrainingOnOwner() noexcept {
    if (state_.load(std::memory_order_relaxed) == SessionState::kClosing) {
        state_.store(SessionState::kDraining, std::memory_order_release);
    }
}

bool Session::TryMarkClosedOnOwner() noexcept {
    auto state = state_.load(std::memory_order_acquire);
    while (state != SessionState::kOpen && state != SessionState::kClosed) {
        if (state_.compare_exchange_weak(state, SessionState::kClosed,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

Session::Session(int fd, IoRing& ring, BufferPool& pool,
                 std::uint32_t send_queue_max_pending)
    : socket_(fd)
    , ring_(ring)
    , pool_(pool)
    , send_queue_(send_queue_max_pending) {}

Session::~Session() = default;

bool Session::RunOnOwner(std::move_only_function<void(Session&)> task) noexcept {
    if (IoRing::Current() == &ring_) {
        task(*this);
        return true;
    }

    auto weak = weak_from_this();
    return ring_.RunOnRing([weak = std::move(weak), task = std::move(task)]() mutable {
        auto owner = weak.lock();
        if (!owner) {
            return;
        }

        auto session = std::static_pointer_cast<Session>(std::move(owner));
        task(*session);
    });
}

void Session::BeginPendingIo() noexcept {
    ++pending_io_;
}

void Session::CompletePendingIo() noexcept {
    if (pending_io_ > 0) {
        --pending_io_;
    }
}

void Session::Start() {
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    CaptureRemoteAddr();
    TouchActivity();
    ring_.Sessions().Add(self);
    OnConnected();
    RegisterRecv();
}

void Session::Disconnect() {
    RunOnOwner([](Session& self) {
        self.DisconnectOnOwner();
    });
}

void Session::DisconnectAfterFlush() {
    RunOnOwner([](Session& self) {
        self.DisconnectAfterFlushOnOwner();
    });
}

void Session::DisconnectAfterFlushOnOwner() {
    if (!CanUseOnOwner()) {
        return;
    }
    disconnect_after_flush_ = true;
    TryDisconnectAfterFlush();
}

void Session::DisconnectOnOwner() {
    if (!TryBeginDisconnectOnOwner()) return;
    obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:ENTER] Disconnect() called", Fd());

    // 1. Shutdown the socket — causes recv to EOF and send to error,
    //    which naturally terminates the multishot recv.
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    auto* disconnect_ev = new (std::nothrow) DisconnectOp(self);
    if (!disconnect_ev) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:OOM_SHUTDOWN] DisconnectEvent allocation failed", Fd());
        TryRelease();
        return;
    }
    disconnect_ev->SetAutoDelete(true);
    BeginPendingIo();
    if (!ring_.PrepDisconnect(*disconnect_ev, Fd())) {
        CompletePendingIo();
        delete disconnect_ev;
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SHUTDOWN] PrepDisconnect failed", Fd());
        TryRelease();
        return;
    }

    // Cancel multishot recv for faster cleanup (best-effort). If PrepCancel
    // fails, shutdown alone will still terminate the multishot by delivering EOF.
    if (active_recv_ev_) {
        auto* cancel_ev = new (std::nothrow) CancelOp(self, active_recv_ev_);
        if (cancel_ev) {
            cancel_ev->SetAutoDelete(true);
            BeginPendingIo();
            if (!ring_.PrepCancel(*active_recv_ev_, cancel_ev)) {
                CompletePendingIo();
                delete cancel_ev;
            }
        }
    }

    MarkDrainingOnOwner();
    TryRelease();
    ring_.Submit();
}

// -- Recv handling (fast/slow path + multishot + ENOBUFS) ----

event::DispatchResult Session::OnRecv(event::RecvEvent&,
                                     std::int32_t res, std::uint32_t flags) {
    const bool more = (flags & IORING_CQE_F_MORE) != 0;
    const bool has_buffer = (flags & IORING_CQE_F_BUFFER) != 0;
    const auto dispatch_result = more
        ? event::DispatchResult::kPending
        : event::DispatchResult::kComplete;

    // No F_MORE means the kernel will send no more CQEs for this SQE.
    if (!more) {
        active_recv_ev_ = nullptr;
    }

    // Always return provided buffer to the ring, even during disconnect.
    // Failing to return leaks buffers from the provided buffer pool.
    if (has_buffer) {
        std::uint16_t buf_id = flags >> IORING_CQE_BUFFER_SHIFT;
        auto& buf_ring = ring_.BufRing();

        if (res > 0 && CanUseOnOwner()) {
            auto view = buf_ring.View(buf_id, static_cast<std::uint32_t>(res));
            TouchActivity();
            if (recv_paused_) {
                if (!QueuePausedRecv(view)) {
                    Disconnect();
                }
            } else {
                OnRecv(view);
            }
        }

        buf_ring.Return(buf_id);
    }

    if (!more) {
        CompletePendingIo();
    }

    // During disconnect, skip normal error handling — just wait for
    // all in-flight ops to settle before releasing manager ownership.
    if (!CanUseOnOwner()) {
        TryRelease();
        return dispatch_result;
    }

    // Normal error handling
    if (res == 0) {
        // Peer closed session
        obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:{}] peer closed connection",
                      Fd(), detail::DisconnectReasonForResult(res));
        Disconnect();
        return dispatch_result;
    }

    if (res == -ENOBUFS) {
        // Buffer pool exhausted — multishot terminated, re-register
        obs::LogWarn(kLogCategory, "Session[fd={}]: [WARN:ENOBUFS] provided buffer pool exhausted, re-registering", Fd());
        if (!recv_paused_) {
            RegisterRecv();
        }
        return dispatch_result;
    }

    if (res < 0) {
        if (detail::IsExpectedDisconnectResult(res)) {
            obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:{}] recv closed res={}",
                          Fd(), detail::DisconnectReasonForResult(res), res);
        } else {
            obs::LogError(kLogCategory, "Session[fd={}]: [DISC:RECV_ERR] recv error res={}",
                          Fd(), res);
        }
        Disconnect();
        return dispatch_result;
    }

    // Multishot ended normally (e.g. internal resource limit) — re-register
    if (!more) {
        if (!recv_paused_) {
            RegisterRecv();
        }
        return dispatch_result;
    }

    return dispatch_result;
}

event::DispatchResult Session::OnDisconnect(event::DisconnectEvent&, std::int32_t) {
    CompletePendingIo();
    TryRelease();
    return event::DispatchResult::kComplete;
}

event::DispatchResult Session::OnCancel(event::CancelEvent&, std::int32_t) {
    CompletePendingIo();
    TryRelease();
    return event::DispatchResult::kComplete;
}

// -- Drain release gate ----

void Session::TryRelease() {
    if (!ClosingStarted() || pending_io_ != 0 || pending_app_io_ != 0)
        return;
    if (!TryMarkClosedOnOwner())
        return;

    obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:RELEASED] session destroyed", Fd());
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    OnDisconnected();

    // Release connected-session ownership. Pending event owners still defer
    // actual destruction as needed.
    ring_.Sessions().Release(self);
}

// -- SQE registration ----

void Session::RegisterRecv() {
    if (!CanUseOnOwner() || recv_paused_) return;
    if (active_recv_ev_) return;
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    auto* recv_ev = new (std::nothrow) RecvOp(self);
    if (!recv_ev) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:OOM_RECV] RecvEvent allocation failed", Fd());
        Disconnect();
        return;
    }
    recv_ev->SetAutoDelete(true);
    BeginPendingIo();
    if (!ring_.PrepRecvMultishot(*recv_ev, Fd())) {
        CompletePendingIo();
        delete recv_ev;
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_RECV] PrepRecvMultishot failed", Fd());
        Disconnect();
        return;
    }
    active_recv_ev_ = recv_ev;
    ring_.Submit();
}

void Session::Tick(std::chrono::steady_clock::time_point now) {
    if (!CanUseOnOwner()) {
        TryRelease();
        return;
    }

    const bool check_timeout =
        timeout_check_interval_.count() > 0 &&
        (next_timeout_check_ == std::chrono::steady_clock::time_point{} ||
         now >= next_timeout_check_);
    if (check_timeout) {
        next_timeout_check_ = now + timeout_check_interval_;
        if (OnTimeoutTick(now)) {
            return;
        }
    }

    if (inactivity_timeout_.count() > 0 &&
        last_activity_ != std::chrono::steady_clock::time_point{} &&
        now - last_activity_ >= inactivity_timeout_ &&
        !HasPendingAppWork()) {
        obs::LogWarn(kLogCategory,
                     "Session[fd={}]: [DISC:INACTIVITY_TIMEOUT] timeout={}ms",
                     Fd(), inactivity_timeout_.count());
        DisconnectOnOwner();
        return;
    }

    if (backpressure_active_ &&
        disconnect_on_high_watermark_ &&
        backpressure_disconnect_delay_.count() > 0 &&
        last_backpressure_high_ != std::chrono::steady_clock::time_point{} &&
        now - last_backpressure_high_ >= backpressure_disconnect_delay_) {
        obs::LogWarn(kLogCategory,
                     "Session[fd={}]: [DISC:BACKPRESSURE_TIMEOUT] delay={}ms",
                     Fd(), backpressure_disconnect_delay_.count());
        DisconnectOnOwner();
    }
}

void Session::SetTimeoutCheckInterval(std::chrono::milliseconds interval) {
    if (interval.count() <= 0) {
        timeout_check_interval_ = std::chrono::milliseconds{0};
        next_timeout_check_ = {};
        return;
    }
    timeout_check_interval_ = interval;
    next_timeout_check_ = std::chrono::steady_clock::now() + interval;
}

void Session::SetInactivityTimeout(std::chrono::milliseconds timeout) {
    inactivity_timeout_ = timeout;
    if (timeout.count() > 0 &&
        (timeout_check_interval_.count() == 0 ||
         timeout_check_interval_ > timeout)) {
        SetTimeoutCheckInterval(std::min(timeout, std::chrono::milliseconds{1000}));
    }
}

void Session::BeginAppIo() {
    ++pending_app_io_;
}

void Session::EndAppIo() {
    if (pending_app_io_ > 0) {
        --pending_app_io_;
    }
    TryDisconnectAfterFlush();
    TryRelease();
}

void Session::PauseRecv() {
    RunOnOwner([](Session& self) {
        if (!self.CanUseOnOwner()) {
            return;
        }
        self.recv_paused_ = true;
    });
}

void Session::ResumeRecv() {
    RunOnOwner([](Session& self) {
        if (!self.CanUseOnOwner()) {
            return;
        }
        self.recv_paused_ = false;
        self.DrainPausedRecv();
        self.RegisterRecv();
    });
}

void Session::CaptureRemoteAddr() {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(Fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        remote_addr_.clear();
        return;
    }

    char host[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        ::inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        port = ntohs(in->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        ::inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
    }

    if (host[0] == '\0') {
        remote_addr_.clear();
        return;
    }
    remote_addr_ = host;
    remote_addr_ += ':';
    remote_addr_ += std::to_string(port);
}

void Session::TouchActivity() noexcept {
    last_activity_ = std::chrono::steady_clock::now();
}

void Session::TryDisconnectAfterFlush() {
    if (!disconnect_after_flush_ || !CanUseOnOwner()) {
        return;
    }
    if (active_send_ev_ != nullptr || send_queue_.Snapshot().current_depth != 0 ||
        HasPendingAppWork()) {
        return;
    }
    DisconnectOnOwner();
}

bool Session::QueuePausedRecv(std::span<const std::byte> data) {
    if (data.empty()) {
        return true;
    }
    const auto next_bytes = paused_recv_bytes_ + data.size();
    if (paused_recv_byte_limit_ != 0 && next_bytes > paused_recv_byte_limit_) {
        obs::LogWarn(kLogCategory,
                     "Session[fd={}]: [DISC:PAUSED_RECV_OVERFLOW] limit={} bytes",
                     Fd(), paused_recv_byte_limit_);
        return false;
    }

    std::vector<std::byte> chunk(data.size());
    std::memcpy(chunk.data(), data.data(), data.size());
    paused_recv_bytes_ = next_bytes;
    paused_recv_queue_.push_back(std::move(chunk));
    return true;
}

void Session::DrainPausedRecv() {
    while (CanUseOnOwner() && !recv_paused_ && !paused_recv_queue_.empty()) {
        auto chunk = std::move(paused_recv_queue_.front());
        paused_recv_queue_.pop_front();
        paused_recv_bytes_ -= chunk.size();
        OnRecv(std::span<const std::byte>(chunk.data(), chunk.size()));
    }
}

} // namespace iouring::net
