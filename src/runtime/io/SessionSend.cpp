#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/IoRing.h>

#include <liburing.h>
#include <iouring_runtime/observability/Logging.h>

#include <cstring>
#include <memory>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring_runtime::core::io {

namespace {
constexpr std::size_t kMaxSendIovecs = 1024;
}

struct Session::SendOpPool {
    std::vector<std::unique_ptr<SendOp>> storage;
    std::vector<SendOp*> free;
};

Session::SendOpPool& Session::SendOpPoolForThread() {
    thread_local SendOpPool pool;
    return pool;
}

Session::SendOp* Session::AcquireSendOp() {
    auto& pool = SendOpPoolForThread();
    if (!pool.free.empty()) {
        auto* op = pool.free.back();
        pool.free.pop_back();
        return op;
    }

    auto op = std::make_unique<SendOp>();
    auto* raw = op.get();
    pool.storage.push_back(std::move(op));
    return raw;
}

void Session::ReleaseSendOp(SendOp* send_op) noexcept {
    if (!send_op) {
        return;
    }

    send_op->msg = {};
    send_op->iovecs.clear();
    send_op->bufs.clear();
    send_op->SetStrongOwner({});
    send_op->SetAutoDelete(false);
    SendOpPoolForThread().free.push_back(send_op);
}

void Session::SendOp::Destroy() noexcept {
    Session::ReleaseSendOp(this);
}

std::expected<void, io::IoError> Session::Send(buffer::SendBufferRef buf) {
    if (disconnecting_) return std::unexpected(IoError::kDisconnected);

    auto result = send_queue_.Push(std::move(buf));
    if (result.overflowed) {
        obs::LogWarn(kLogCategory, "Session[fd={}]: [DISC:SEND_OVERFLOW] send queue overflow", Fd());
        if (on_send_overflow_)
            on_send_overflow_(std::static_pointer_cast<Session>(shared_from_this()));
        Disconnect();
        return std::unexpected(IoError::kSendFailed);
    }
    UpdateBackpressureState(result.current_depth, result.current_bytes, true);
    if (result.needs_register) {
        RegisterSend();
    }
    return {};
}

void Session::OnSend(ring::SendEvent& ev, std::int32_t res) {
    auto& send_op = static_cast<SendOp&>(ev);
    --pending_io_;
    if (&send_op == active_send_ev_) {
        active_send_ev_ = nullptr;
    }

    if (disconnecting_) {
        TryRelease();
        return;
    }

    if (res < 0) {
        if (IsExpectedDisconnectResult(res)) {
            obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:{}] send closed res={}",
                          Fd(), DisconnectReasonForResult(res), res);
        } else {
            obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SEND_ERR] send error res={}",
                          Fd(), res);
        }
        Disconnect();
        return;
    }

    std::size_t requested = 0;
    for (const auto& iov : send_op.iovecs) requested += iov.iov_len;
    const auto sent = static_cast<std::size_t>(res);

    if (sent < requested) {
        AdvanceSendState(send_op.iovecs, send_op.bufs, sent);

        std::memset(&send_op.msg, 0, sizeof(send_op.msg));
        send_op.msg.msg_iov = send_op.iovecs.data();
        send_op.msg.msg_iovlen = send_op.iovecs.size();

        ++pending_io_;
        if (!ring_.PrepSendMsg(send_op, Fd(), &send_op.msg, MSG_NOSIGNAL)) {
            --pending_io_;
            obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SEND_RESUME] partial send resume failed", Fd());
            Disconnect();
            return;
        }
        active_send_ev_ = &send_op;
        send_op.RetainAfterDispatch();
        ring_.Submit();
        return;
    }

    send_queue_.MarkSent();
    std::vector<SendBufferRef> next_bufs;
    if (send_queue_.DrainInto(next_bufs, kMaxSendIovecs) != 0) {
        SendBatch(std::move(next_bufs));
        return;
    }

    UpdateBackpressureState();
    OnSocketDrained();
    MaybeDisconnectAfterFlush();
}

void Session::UpdateBackpressureState(std::size_t depth_hint,
                                      std::size_t bytes_hint,
                                      bool hint_valid) {
    if (backpressure_high_watermark_ == 0 && backpressure_high_bytes_ == 0) {
        return;
    }

    const auto stats = hint_valid ? buffer::SendQueue::Stats{
        .current_depth = depth_hint,
        .pending_bytes = bytes_hint,
    } : send_queue_.Snapshot();
    const bool depth_high = backpressure_high_watermark_ != 0 &&
                            stats.current_depth >= backpressure_high_watermark_;
    const bool bytes_high = backpressure_high_bytes_ != 0 &&
                            stats.pending_bytes >= backpressure_high_bytes_;
    const bool depth_low = backpressure_high_watermark_ == 0 ||
                           stats.current_depth <= backpressure_low_watermark_;
    const bool bytes_low = backpressure_high_bytes_ == 0 ||
                           stats.pending_bytes <= backpressure_low_bytes_;

    if (!backpressure_active_ && (depth_high || bytes_high)) {
        backpressure_active_ = true;
        backpressure_active_since_ = std::chrono::steady_clock::now();
        if (pause_recv_on_backpressure_) {
            PauseRecv();
        }
        if (on_backpressure_) {
            on_backpressure_(std::static_pointer_cast<Session>(shared_from_this()), true);
        }
        if (disconnect_on_high_watermark_ && !disconnecting_ &&
            backpressure_disconnect_delay_.count() == 0) {
            obs::LogWarn(kLogCategory,
                         "Session[fd={}]: [DISC:SLOW_CLIENT] send queue pressure depth={} bytes={}",
                         Fd(), stats.current_depth, stats.pending_bytes);
            Disconnect();
        }
        return;
    }

    if (backpressure_active_ && depth_low && bytes_low) {
        backpressure_active_ = false;
        backpressure_active_since_ = {};
        if (pause_recv_on_backpressure_) {
            ResumeRecv();
        }
        if (on_backpressure_) {
            on_backpressure_(std::static_pointer_cast<Session>(shared_from_this()), false);
        }
    }
}

void Session::RegisterSend() {
    if (disconnecting_) return;

    if (active_send_ev_ != nullptr) return;

    std::vector<SendBufferRef> bufs;
    if (send_queue_.DrainInto(bufs, kMaxSendIovecs) == 0) return;

    SendBatch(std::move(bufs));
}

void Session::AdvanceSendState(std::vector<struct iovec>& iovs,
                               std::vector<buffer::SendBufferRef>& bufs,
                               std::size_t advanced) {
    std::size_t consumed = 0;
    while (consumed < iovs.size() && advanced >= iovs[consumed].iov_len) {
        advanced -= iovs[consumed].iov_len;
        ++consumed;
    }
    if (consumed > 0) {
        iovs.erase(iovs.begin(), iovs.begin() + consumed);
        bufs.erase(bufs.begin(), bufs.begin() + consumed);
    }
    if (advanced > 0 && !iovs.empty()) {
        auto& front = iovs.front();
        front.iov_base = static_cast<std::byte*>(front.iov_base) + advanced;
        front.iov_len -= advanced;
    }
}

void Session::SendBatch(std::vector<SendBufferRef> bufs) {
    if (disconnecting_ || bufs.empty()) return;

    if (active_send_ev_ != nullptr) return;

    auto* send_op = AcquireSendOp();
    if (!send_op) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:OOM_SEND] SendOp allocation failed", Fd());
        Disconnect();
        return;
    }
    send_op->SetStrongOwner(std::static_pointer_cast<Session>(shared_from_this()));
    send_op->SetAutoDelete(true);
    send_op->bufs = std::move(bufs);
    SendInFlightBatch(*send_op);
}

void Session::SendInFlightBatch(SendOp& send_op) {
    if (disconnecting_ || send_op.bufs.empty()) return;

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

    ++pending_io_;
    if (!ring_.PrepSendMsg(send_op, Fd(), &send_op.msg, MSG_NOSIGNAL)) {
        --pending_io_;
        ReleaseSendOp(&send_op);
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SEND] PrepSendMsg failed", Fd());
        Disconnect();
        return;
    }
    active_send_ev_ = &send_op;
    ring_.Submit();
}

} // namespace iouring_runtime::core::io
