#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/SessionDetail.h>

#include <liburing.h>
#include <iouring_runtime/observability/Logging.h>

#include <cerrno>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring_runtime::core::io {

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

void Session::MarkClosedOnOwner() noexcept {
    state_.store(SessionState::kClosed, std::memory_order_release);
}

Session::Session(int fd, IoRing& ring, BufferPool& pool,
                 std::uint32_t send_queue_max_pending)
    : socket_(fd)
    , ring_(ring)
    , pool_(pool)
    , send_queue_(send_queue_max_pending) {
    drain_gate_.SetOnDrained([this] {
        TryRelease();
    });
}

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

ring::DrainGate::Token Session::EnterDrain() {
    return drain_gate_.Enter();
}

void Session::Start() {
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    ring_.Sessions().Add(self);
    OnConnected();
    RegisterRecv();
}

void Session::Disconnect() {
    RunOnOwner([](Session& self) {
        self.DisconnectOnOwner();
    });
}

void Session::DisconnectOnOwner() {
    if (!TryBeginDisconnectOnOwner()) return;
    obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:ENTER] Disconnect() called", Fd());

    // 1. Shutdown the socket — causes recv to EOF and send to error,
    //    which naturally terminates the multishot recv.
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    auto* disconnect_ev = new (std::nothrow) ring::DisconnectEvent();
    if (!disconnect_ev) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:OOM_SHUTDOWN] DisconnectEvent allocation failed", Fd());
        drain_gate_.Close();
        TryRelease();
        return;
    }
    disconnect_ev->SetStrongOwner(self);
    disconnect_ev->SetAutoDelete(true);
    disconnect_ev->SetDrainToken(EnterDrain());
    if (!ring_.PrepDisconnect(*disconnect_ev, Fd())) {
        delete disconnect_ev;
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SHUTDOWN] PrepDisconnect failed", Fd());
        drain_gate_.Close();
        TryRelease();
        return;
    }

    // Cancel multishot recv for faster cleanup (best-effort). If PrepCancel
    // fails, shutdown alone will still terminate the multishot by delivering EOF.
    if (active_recv_ev_) {
        auto* cancel_ev = new (std::nothrow) ring::CancelEvent(active_recv_ev_);
        if (cancel_ev) {
            cancel_ev->SetStrongOwner(self);
            cancel_ev->SetAutoDelete(true);
            cancel_ev->SetDrainToken(EnterDrain());
            if (!ring_.PrepCancel(*active_recv_ev_, cancel_ev)) {
                delete cancel_ev;
            }
        }
    }

    MarkDrainingOnOwner();
    drain_gate_.Close();
    ring_.Submit();
}

// -- Recv handling (fast/slow path + multishot + ENOBUFS) ----

void Session::OnRecv(ring::RecvEvent&,
                     std::int32_t res, std::uint32_t flags) {
    const bool more = (flags & IORING_CQE_F_MORE) != 0;
    const bool has_buffer = (flags & IORING_CQE_F_BUFFER) != 0;

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
            OnRecv(view);
        }

        buf_ring.Return(buf_id);
    }

    // During disconnect, skip normal error handling — just wait for
    // all in-flight ops to settle before releasing manager ownership.
    if (!CanUseOnOwner()) {
        TryRelease();
        return;
    }

    // Normal error handling
    if (res == 0) {
        // Peer closed session
        obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:{}] peer closed connection",
                      Fd(), detail::DisconnectReasonForResult(res));
        Disconnect();
        return;
    }

    if (res == -ENOBUFS) {
        // Buffer pool exhausted — multishot terminated, re-register
        obs::LogWarn(kLogCategory, "Session[fd={}]: [WARN:ENOBUFS] provided buffer pool exhausted, re-registering", Fd());
        RegisterRecv();
        return;
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
        return;
    }

    // Multishot ended normally (e.g. internal resource limit) — re-register
    if (!more)
        RegisterRecv();
}

void Session::OnDisconnect(ring::DisconnectEvent&, std::int32_t) {
    TryRelease();
}

void Session::OnCancel(ring::CancelEvent&, std::int32_t) {
    TryRelease();
}

// -- Drain release gate ----

void Session::TryRelease() {
    if (!ClosingStarted() || !drain_gate_.Drained())
        return;
    if (disconnected_notified_)
        return;
    disconnected_notified_ = true;
    MarkClosedOnOwner();

    obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:RELEASED] session destroyed", Fd());
    auto self = std::static_pointer_cast<Session>(shared_from_this());
    OnDisconnected();

    // Release connected-session ownership. Pending event owners still defer
    // actual destruction as needed.
    ring_.Sessions().Release(self);
}

// -- SQE registration ----

void Session::RegisterRecv() {
    if (!CanUseOnOwner()) return;
    if (active_recv_ev_) return;
    auto* recv_ev = new (std::nothrow) ring::RecvEvent();
    if (!recv_ev) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:OOM_RECV] RecvEvent allocation failed", Fd());
        Disconnect();
        return;
    }
    recv_ev->SetStrongOwner(std::static_pointer_cast<Session>(shared_from_this()));
    recv_ev->SetAutoDelete(true);
    recv_ev->SetDrainToken(EnterDrain());
    if (!ring_.PrepRecvMultishot(*recv_ev, Fd())) {
        delete recv_ev;
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_RECV] PrepRecvMultishot failed", Fd());
        Disconnect();
        return;
    }
    active_recv_ev_ = recv_ev;
    ring_.Submit();
}

} // namespace iouring_runtime::core::io
