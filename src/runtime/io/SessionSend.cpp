#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/SessionDetail.h>

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
    if (ring::IoRing::Current() == &ring_) {
        return SendOnOwner(std::move(buf));
    }

    if (!RunOnOwner([buf = std::move(buf)](Session& self) mutable {
            (void)self.SendOnOwner(std::move(buf));
        })) {
        return std::unexpected(IoError::kSendFailed);
    }

    return {};
}

std::expected<void, io::IoError> Session::SendOnOwner(buffer::SendBufferRef buf) {
    if (!CanUseOnOwner()) return std::unexpected(IoError::kDisconnected);

    auto result = send_queue_.Push(std::move(buf));
    if (result.overflowed) {
        obs::LogWarn(kLogCategory, "Session[fd={}]: [DISC:SEND_OVERFLOW] send queue overflow", Fd());
        Disconnect();
        return std::unexpected(IoError::kSendFailed);
    }
    if (result.needs_register) {
        RegisterSend();
    }
    return {};
}

void Session::OnSend(ring::SendEvent& ev, std::int32_t res) {
    auto& send_op = static_cast<SendOp&>(ev);
    if (&send_op == active_send_ev_) {
        active_send_ev_ = nullptr;
    }

    if (!CanUseOnOwner()) {
        TryRelease();
        return;
    }

    if (res < 0) {
        if (detail::IsExpectedDisconnectResult(res)) {
            obs::LogDebug(kLogCategory, "Session[fd={}]: [DISC:{}] send closed res={}",
                          Fd(), detail::DisconnectReasonForResult(res), res);
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
        detail::AdvanceSendState(send_op.iovecs, send_op.bufs, sent);

        std::memset(&send_op.msg, 0, sizeof(send_op.msg));
        send_op.msg.msg_iov = send_op.iovecs.data();
        send_op.msg.msg_iovlen = send_op.iovecs.size();

        if (!ring_.PrepSendMsg(send_op, Fd(), &send_op.msg, MSG_NOSIGNAL)) {
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
}

void Session::RegisterSend() {
    if (!CanUseOnOwner()) return;

    if (active_send_ev_ != nullptr) return;

    std::vector<SendBufferRef> bufs;
    if (send_queue_.DrainInto(bufs, kMaxSendIovecs) == 0) return;

    SendBatch(std::move(bufs));
}

void Session::SendBatch(std::vector<SendBufferRef> bufs) {
    if (!CanUseOnOwner() || bufs.empty()) return;

    if (active_send_ev_ != nullptr) return;

    auto* send_op = new (std::nothrow) SendOp();
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

    send_op.SetDrainToken(EnterDrain());
    if (!ring_.PrepSendMsg(send_op, Fd(), &send_op.msg, MSG_NOSIGNAL)) {
        delete &send_op;
        obs::LogError(kLogCategory, "Session[fd={}]: [DISC:SQE_FULL_SEND] PrepSendMsg failed", Fd());
        Disconnect();
        return;
    }
    active_send_ev_ = &send_op;
    ring_.Submit();
}

} // namespace iouring_runtime::core::io
