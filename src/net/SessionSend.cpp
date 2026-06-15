#include <iouring/net/Session.h>
#include <iouring/event/IoRing.h>
#include <iouring/net/SessionDetail.h>

#include <liburing.h>
#include <iouring/observability/Logging.h>

#include <cstring>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring::net {

namespace {
constexpr std::size_t kMaxSendIovecs = 1024;
}

std::expected<void, IoError> Session::Send(buffer::SendBufferRef buf) {
    if (event::IoRing::Current() == &ring_) {
        return SendOnOwner(std::move(buf));
    }

    if (!RunOnOwner([buf = std::move(buf)](Session& self) mutable {
            (void)self.SendOnOwner(std::move(buf));
        })) {
        return std::unexpected(IoError::kSendFailed);
    }

    return {};
}

std::expected<void, IoError> Session::SendOnOwner(buffer::SendBufferRef buf) {
    if (!CanUseOnOwner()) return std::unexpected(IoError::kDisconnected);

    auto result = send_queue_.Push(std::move(buf));
    if (result.overflowed) {
        obs::LogWarn(kLogCategory, "Session[fd={}]: [DISC:SEND_OVERFLOW] send queue overflow", Fd());
        Disconnect();
        return std::unexpected(IoError::kSendFailed);
    }
    UpdateBackpressure(send_queue_.Snapshot());
    if (result.needs_register) {
        RegisterSend();
    }
    return {};
}

event::DispatchResult Session::OnSend(event::SendEvent& ev, std::int32_t res) {
    auto& send_op = static_cast<SendOp&>(ev);
    if (&send_op == active_send_ev_) {
        active_send_ev_ = nullptr;
    }

    if (!CanUseOnOwner()) {
        CompletePendingIo();
        TryRelease();
        return event::DispatchResult::kComplete;
    }

    if (res < 0) {
        if (detail::IsExpectedDisconnectResult(res)) {
            obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:{}] send closed res={}",
                          Fd(), detail::DisconnectReasonForResult(res), res);
        } else {
            obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SEND_ERR] send error res={}",
                          Fd(), res);
        }
        CompletePendingIo();
        Disconnect();
        return event::DispatchResult::kComplete;
    }

    std::size_t requested = 0;
    for (const auto& iov : send_op.iovecs) requested += iov.iov_len;
    const auto sent = static_cast<std::size_t>(res);

    if (sent < requested) {
        detail::AdvanceSendState(send_op.iovecs, send_op.bufs, sent);

        std::memset(&send_op.msg, 0, sizeof(send_op.msg));
        send_op.msg.msg_iov = send_op.iovecs.data();
        send_op.msg.msg_iovlen = send_op.iovecs.size();

        if (!ring_.PrepSendMsg(send_op, Fd(), &send_op.msg, MSG_NOSIGNAL)) {
            obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SEND_RESUME] partial send resume failed", Fd());
            CompletePendingIo();
            Disconnect();
            return event::DispatchResult::kComplete;
        }
        active_send_ev_ = &send_op;
        ring_.Submit();
        return event::DispatchResult::kPending;
    }

    send_queue_.MarkSent();
    CompletePendingIo();
    UpdateBackpressure(send_queue_.Snapshot());
    OnSocketDrained();
    TryDisconnectAfterFlush();
    if (active_send_ev_ != nullptr) {
        return event::DispatchResult::kComplete;
    }

    std::vector<SendBufferRef> next_bufs;
    if (send_queue_.DrainInto(next_bufs, kMaxSendIovecs) != 0) {
        UpdateBackpressure(send_queue_.Snapshot());
        SendBatch(std::move(next_bufs));
        return event::DispatchResult::kComplete;
    }
    TryDisconnectAfterFlush();
    return event::DispatchResult::kComplete;
}

void Session::RegisterSend() {
    if (!CanUseOnOwner()) return;

    if (active_send_ev_ != nullptr) return;

    std::vector<SendBufferRef> bufs;
    if (send_queue_.DrainInto(bufs, kMaxSendIovecs) == 0) return;
    UpdateBackpressure(send_queue_.Snapshot());

    SendBatch(std::move(bufs));
}

void Session::SendBatch(std::vector<SendBufferRef> bufs) {
    if (!CanUseOnOwner() || bufs.empty()) return;

    if (active_send_ev_ != nullptr) return;

    auto self = std::static_pointer_cast<Session>(shared_from_this());
    auto* send_op = new (std::nothrow) SendOp(self);
    if (!send_op) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:OOM_SEND] SendOp allocation failed", Fd());
        Disconnect();
        return;
    }
    send_op->SetAutoDelete(true);
    send_op->bufs = std::move(bufs);
    SendInFlightBatch(*send_op);
}

void Session::SendInFlightBatch(SendOp& send_op) {
    if (!CanUseOnOwner() || send_op.bufs.empty()) return;

    auto& bufs = send_op.bufs;
    send_op.iovecs.resize(bufs.size());
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        auto data = bufs[i]->Data();
        send_op.iovecs[i].iov_base = const_cast<std::byte*>(data.data());
        send_op.iovecs[i].iov_len = data.size();
    }

    std::memset(&send_op.msg, 0, sizeof(send_op.msg));
    send_op.msg.msg_iov = send_op.iovecs.data();
    send_op.msg.msg_iovlen = send_op.iovecs.size();

    BeginPendingIo();
    if (!ring_.PrepSendMsg(send_op, Fd(), &send_op.msg, MSG_NOSIGNAL)) {
        CompletePendingIo();
        delete &send_op;
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SEND] PrepSendMsg failed", Fd());
        Disconnect();
        return;
    }
    active_send_ev_ = &send_op;
    ring_.Submit();
}

bool Session::HighWatermarkReached(
    const buffer::SendQueue::Stats& stats) const noexcept {
    return (send_queue_high_watermark_ != 0 &&
            stats.current_depth >= send_queue_high_watermark_) ||
           (send_queue_high_bytes_ != 0 &&
            stats.pending_bytes >= send_queue_high_bytes_);
}

bool Session::LowWatermarkReached(
    const buffer::SendQueue::Stats& stats) const noexcept {
    const bool depth_low =
        send_queue_high_watermark_ == 0 ||
        stats.current_depth <= send_queue_low_watermark_;
    const bool bytes_low =
        send_queue_high_bytes_ == 0 ||
        stats.pending_bytes <= send_queue_low_bytes_;
    return depth_low && bytes_low;
}

void Session::UpdateBackpressure(const buffer::SendQueue::Stats& stats) {
    if (!backpressure_active_) {
        if (!HighWatermarkReached(stats)) {
            return;
        }

        backpressure_active_ = true;
        last_backpressure_high_ = std::chrono::steady_clock::now();
        OnBackpressure(true);
        if (pause_recv_on_backpressure_) {
            recv_paused_ = true;
        }
        if (disconnect_on_high_watermark_ &&
            backpressure_disconnect_delay_.count() == 0) {
            obs::LogWarn(kLogCategory,
                         "Session[fd={}]: [DISC:SEND_HIGH_WATERMARK] depth={} bytes={}",
                         Fd(), stats.current_depth, stats.pending_bytes);
            Disconnect();
        }
        return;
    }

    if (!LowWatermarkReached(stats)) {
        return;
    }

    backpressure_active_ = false;
    last_backpressure_high_ = {};
    OnBackpressure(false);
    if (pause_recv_on_backpressure_) {
        recv_paused_ = false;
        DrainPausedRecv();
        RegisterRecv();
    }
}

} // namespace iouring::net
