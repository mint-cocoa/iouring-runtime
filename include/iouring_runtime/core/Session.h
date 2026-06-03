#pragma once

#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/SendQueue.h>
#include <iouring_runtime/core/SocketHandle.h>
#include <iouring_runtime/core/Error.h>
#include <iouring_runtime/core/RingEvent.h>
#include <iouring_runtime/core/Types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <sys/uio.h>

namespace iouring_runtime::core::ring { class IoRing; }

namespace iouring_runtime::core::io {

using buffer::BufferPool;
using buffer::SendBufferRef;
using ring::IoRing;

class Session;
using SessionRef = std::shared_ptr<Session>;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(int fd, IoRing& ring, BufferPool& pool,
            std::uint32_t send_queue_max_pending = 4096);
    virtual ~Session();

    void Start();
    std::expected<void, io::IoError> Send(buffer::SendBufferRef buf);
    void Disconnect();
    void DisconnectAfterFlush();
    void Tick(std::chrono::steady_clock::time_point now);

    bool Disconnecting() const noexcept { return ClosingStarted(); }
    int Fd() const { return socket_.Get(); }
    core::SessionId GetSessionId() const noexcept { return session_id_; }

protected:
    virtual void OnRecv(std::span<const std::byte> data) = 0;
    virtual void OnConnected() {}
    virtual void OnDisconnected() {}
    virtual bool HasPendingAppWork() const { return false; }
    virtual bool OnTimeoutTick(std::chrono::steady_clock::time_point) { return false; }
    virtual void OnSocketDrained() {}
    virtual void OnBackpressure(bool) {}

    BufferPool& Pool() { return pool_; }
    IoRing& Ring() { return ring_; }
    core::SessionId SessionId() const noexcept { return session_id_; }
    std::string_view RemoteAddr() const noexcept { return remote_addr_; }
    void SetTimeoutCheckInterval(std::chrono::milliseconds interval);
    void SetInactivityTimeout(std::chrono::milliseconds timeout);
    void BeginAppIo();
    void EndAppIo();
    void PauseRecv();
    void ResumeRecv();
    bool BackpressureActive() const noexcept { return backpressure_active_; }

private:
    friend class SessionControl;

    struct SendOp;
    enum class SessionState : std::uint8_t {
        kOpen,
        kClosing,
        kDraining,
        kClosed,
    };

    bool RunOnOwner(std::move_only_function<void(Session&)> task) noexcept;
    void DisconnectOnOwner();
    void DisconnectAfterFlushOnOwner();
    void RegisterRecv();
    void RegisterSend();
    std::expected<void, io::IoError> SendOnOwner(buffer::SendBufferRef buf);
    void SendBatch(std::vector<SendBufferRef> bufs);
    void SendInFlightBatch(SendOp& send_op);
    ring::DispatchResult OnRecv(ring::RecvEvent& ev, std::int32_t result, std::uint32_t flags);
    ring::DispatchResult OnSend(ring::SendEvent& ev, std::int32_t result);
    ring::DispatchResult OnDisconnect(ring::DisconnectEvent& ev, std::int32_t result);
    ring::DispatchResult OnCancel(ring::CancelEvent& ev, std::int32_t result);

    // Release manager ownership when all in-flight I/O has settled.
    void TryRelease();
    void BeginPendingIo() noexcept;
    void CompletePendingIo() noexcept;
    bool ClosingStarted() const noexcept;
    bool CanUseOnOwner() const noexcept;
    bool TryBeginDisconnectOnOwner() noexcept;
    void MarkDrainingOnOwner() noexcept;
    bool TryMarkClosedOnOwner() noexcept;
    void CaptureRemoteAddr();
    void TouchActivity() noexcept;
    void TryDisconnectAfterFlush();
    bool QueuePausedRecv(std::span<const std::byte> data);
    void DrainPausedRecv();
    void UpdateBackpressure(const buffer::SendQueue::Stats& stats);
    bool HighWatermarkReached(const buffer::SendQueue::Stats& stats) const noexcept;
    bool LowWatermarkReached(const buffer::SendQueue::Stats& stats) const noexcept;

    SocketHandle socket_;
    IoRing& ring_;
    BufferPool& pool_;
    buffer::SendQueue send_queue_;
    std::atomic<SessionState> state_{SessionState::kOpen};
    int pending_io_ = 0;
    int pending_app_io_ = 0;
    core::SessionId session_id_{0};
    std::string remote_addr_;
    std::chrono::steady_clock::time_point last_activity_{};
    std::chrono::steady_clock::time_point last_backpressure_high_{};
    std::chrono::steady_clock::time_point next_timeout_check_{};
    std::chrono::milliseconds inactivity_timeout_{0};
    std::chrono::milliseconds timeout_check_interval_{0};
    std::uint32_t send_queue_high_watermark_{0};
    std::uint32_t send_queue_low_watermark_{0};
    std::size_t send_queue_high_bytes_{0};
    std::size_t send_queue_low_bytes_{0};
    std::chrono::milliseconds backpressure_disconnect_delay_{0};
    std::size_t paused_recv_byte_limit_{1024 * 1024};
    std::size_t paused_recv_bytes_{0};
    std::deque<std::vector<std::byte>> paused_recv_queue_;
    bool disconnect_after_flush_{false};
    bool recv_paused_{false};
    bool backpressure_active_{false};
    bool pause_recv_on_backpressure_{false};
    bool disconnect_on_high_watermark_{false};

    // Active io_uring ops whose user_data points at heap operation contexts.
    // The pointed-to objects own a strong Session reference and are deleted
    // by IoRing after their final CQE.
    struct RecvOp final : ring::RecvEvent {
        explicit RecvOp(SessionRef owner) : owner_(std::move(owner)) {}

        ring::DispatchResult Dispatch(std::int32_t result, std::uint32_t flags) override {
            return owner_->OnRecv(*this, result, flags);
        }

    private:
        SessionRef owner_;
    };

    struct DisconnectOp final : ring::DisconnectEvent {
        explicit DisconnectOp(SessionRef owner) : owner_(std::move(owner)) {}

        ring::DispatchResult Dispatch(std::int32_t result, std::uint32_t) override {
            return owner_->OnDisconnect(*this, result);
        }

    private:
        SessionRef owner_;
    };

    struct CancelOp final : ring::CancelEvent {
        explicit CancelOp(SessionRef owner, ring::IoEvent* target = nullptr)
            : ring::CancelEvent(target)
            , owner_(std::move(owner)) {}

        ring::DispatchResult Dispatch(std::int32_t result, std::uint32_t) override {
            return owner_->OnCancel(*this, result);
        }

    private:
        SessionRef owner_;
    };

    RecvOp* active_recv_ev_ = nullptr;

    struct SendOp final : ring::SendEvent {
        explicit SendOp(SessionRef owner) : owner_(std::move(owner)) {}

        ring::DispatchResult Dispatch(std::int32_t result, std::uint32_t) override {
            return owner_->OnSend(*this, result);
        }

        struct msghdr msg{};
        std::vector<struct iovec> iovecs;
        std::vector<SendBufferRef> bufs;

    private:
        SessionRef owner_;
    };

    SendOp* active_send_ev_ = nullptr;
};

} // namespace iouring_runtime::core::io
