#pragma once

#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/SendQueue.h>
#include <iouring_runtime/core/SocketHandle.h>
#include <iouring_runtime/core/Error.h>
#include <iouring_runtime/core/EventHandler.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <sys/uio.h>

namespace iouring_runtime::core::ring { class IoRing; }

namespace iouring_runtime::core::io {

using buffer::BufferPool;
using buffer::SendBufferRef;
using ring::IoRing;

class Session;
using SessionRef = std::shared_ptr<Session>;

class Session : public ring::EventHandler {
public:
    Session(int fd, IoRing& ring, BufferPool& pool,
            std::uint32_t send_queue_max_pending = 4096);
    ~Session() override;

    void Start();
    std::expected<void, io::IoError> Send(buffer::SendBufferRef buf);
    void Disconnect();

    bool Disconnecting() const noexcept { return ClosingStarted(); }
    int Fd() const { return socket_.Get(); }

protected:
    virtual void OnRecv(std::span<const std::byte> data) = 0;
    virtual void OnConnected() {}
    virtual void OnDisconnected() {}

    BufferPool& Pool() { return pool_; }
    IoRing& Ring() { return ring_; }

private:
    struct SendOp;
    enum class SessionState : std::uint8_t {
        kOpen,
        kClosing,
        kDraining,
        kClosed,
    };

    bool RunOnOwner(std::move_only_function<void(Session&)> task) noexcept;
    void DisconnectOnOwner();
    void RegisterRecv();
    void RegisterSend();
    std::expected<void, io::IoError> SendOnOwner(buffer::SendBufferRef buf);
    void SendBatch(std::vector<SendBufferRef> bufs);
    void SendInFlightBatch(SendOp& send_op);
    void OnRecv(ring::RecvEvent& ev, std::int32_t result, std::uint32_t flags) override;
    void OnSend(ring::SendEvent& ev, std::int32_t result) override;
    void OnDisconnect(ring::DisconnectEvent& ev, std::int32_t result) override;
    void OnCancel(ring::CancelEvent& ev, std::int32_t result) override;

    // Release manager ownership when all in-flight I/O has settled.
    void TryRelease();
    ring::DrainGate::Token EnterDrain();
    bool ClosingStarted() const noexcept;
    bool CanUseOnOwner() const noexcept;
    bool TryBeginDisconnectOnOwner() noexcept;
    void MarkDrainingOnOwner() noexcept;
    void MarkClosedOnOwner() noexcept;

    SocketHandle socket_;
    IoRing& ring_;
    BufferPool& pool_;
    buffer::SendQueue send_queue_;
    std::atomic<SessionState> state_{SessionState::kOpen};
    bool disconnected_notified_ = false;

    // Drain gate for submitted ops that must settle before OnDisconnected()
    // and connected-session ownership release.
    ring::DrainGate drain_gate_;

    // Active io_uring ops whose user_data points at heap operation contexts.
    // The pointed-to objects own a strong Session reference and are deleted
    // by IoRing after their final CQE.
    ring::RecvEvent* active_recv_ev_ = nullptr;

    struct SendOp final : ring::SendEvent {
        struct msghdr msg{};
        std::vector<struct iovec> iovecs;
        std::vector<SendBufferRef> bufs;
    };

    SendOp* active_send_ev_ = nullptr;
};

} // namespace iouring_runtime::core::io
