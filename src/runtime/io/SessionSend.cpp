#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/IoRing.h>

#include <liburing.h>
#include <iouring_runtime/observability/Logging.h>

#include <cstring>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring_runtime::core::io {

namespace {
constexpr std::size_t kMaxSendIovecs = 1024;
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

void Session::OnSend(ring::SendEvent&, std::int32_t res) {
    --pending_io_;

    if (disconnecting_) {
        in_flight_bufs_.clear();
        send_iovecs_.clear();
        TryRelease();
        return;
    }

    if (res < 0) {
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SEND_ERR] send error res={}", Fd(), res);
        in_flight_bufs_.clear();
        send_iovecs_.clear();
        Disconnect();
        return;
    }

    std::size_t requested = 0;
    for (const auto& iov : send_iovecs_) requested += iov.iov_len;
    const auto sent = static_cast<std::size_t>(res);

    if (sent < requested) {
        AdvanceSendState(send_iovecs_, in_flight_bufs_, sent);

        std::memset(&send_msg_, 0, sizeof(send_msg_));
        send_msg_.msg_iov = send_iovecs_.data();
        send_msg_.msg_iovlen = send_iovecs_.size();

        ++pending_io_;
        if (!ring_.PrepSendMsg(send_ev_, Fd(), &send_msg_, MSG_NOSIGNAL)) {
            --pending_io_;
            in_flight_bufs_.clear();
            send_iovecs_.clear();
            obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SEND_RESUME] partial send resume failed", Fd());
            Disconnect();
            return;
        }
        ring_.Submit();
        return;
    }

    in_flight_bufs_.clear();
    send_iovecs_.clear();

    send_queue_.MarkSent();
    if (send_queue_.DrainInto(in_flight_bufs_, kMaxSendIovecs) != 0) {
        SendInFlightBatch();
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
        if (on_backpressure_) {
            on_backpressure_(std::static_pointer_cast<Session>(shared_from_this()), true);
        }
        if (disconnect_on_high_watermark_ && !disconnecting_) {
            obs::LogWarn(kLogCategory,
                         "Session[fd={}]: [DISC:SLOW_CLIENT] send queue pressure depth={} bytes={}",
                         Fd(), stats.current_depth, stats.pending_bytes);
            Disconnect();
        }
        return;
    }

    if (backpressure_active_ && depth_low && bytes_low) {
        backpressure_active_ = false;
        if (on_backpressure_) {
            on_backpressure_(std::static_pointer_cast<Session>(shared_from_this()), false);
        }
    }
}

void Session::RegisterSend() {
    if (disconnecting_) return;

    if (send_queue_.DrainInto(in_flight_bufs_, kMaxSendIovecs) == 0) return;

    SendInFlightBatch();
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

    in_flight_bufs_ = std::move(bufs);
    SendInFlightBatch();
}

void Session::SendInFlightBatch() {
    if (disconnecting_ || in_flight_bufs_.empty()) return;

    auto& bufs = in_flight_bufs_;
    send_iovecs_.resize(bufs.size());
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        auto data = bufs[i]->Data();
        send_iovecs_[i].iov_base = const_cast<std::byte*>(data.data());
        send_iovecs_[i].iov_len = data.size();
    }

    std::memset(&send_msg_, 0, sizeof(send_msg_));
    send_msg_.msg_iov = send_iovecs_.data();
    send_msg_.msg_iovlen = send_iovecs_.size();

    ++pending_io_;
    if (!ring_.PrepSendMsg(send_ev_, Fd(), &send_msg_, MSG_NOSIGNAL)) {
        --pending_io_;
        in_flight_bufs_.clear();
        send_iovecs_.clear();
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SEND] PrepSendMsg failed", Fd());
        Disconnect();
        return;
    }
    ring_.Submit();
}

} // namespace iouring_runtime::core::io
