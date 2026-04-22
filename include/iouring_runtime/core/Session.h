#pragma once

#include <iouring_runtime/core/RecvBuffer.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/SendQueue.h>
#include <iouring_runtime/core/SocketHandle.h>
#include <iouring_runtime/core/Types.h>
#include <iouring_runtime/core/Error.h>
#include <iouring_runtime/core/EventHandler.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>
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
    using ConnectedCallback = std::move_only_function<void(SessionRef)>;
    using DisconnectCallback = std::move_only_function<void(SessionRef)>;
    using SendOverflowCallback = std::move_only_function<void(SessionRef)>;
    using BackpressureCallback = std::move_only_function<void(SessionRef, bool active)>;

    Session(int fd, IoRing& ring, BufferPool& pool,
            std::uint32_t send_queue_max_pending = 4096);
    ~Session() override;

    void Start();
    std::expected<void, io::IoError> Send(buffer::SendBufferRef buf);
    void Disconnect();
    void DisconnectAfterFlush();
    void ReleaseOwnership();

    // Enable an io_uring TIMEOUT-driven inactivity watchdog. The session
    // is disconnected when (now - last successful recv) exceeds `timeout`.
    // Passing zero (the default) disables the watchdog. Must be called
    // before Start().
    void SetInactivityTimeout(std::chrono::milliseconds timeout);

    // Enable periodic timeout ticks for protocol-level deadlines implemented
    // by subclasses. Passing zero disables protocol-only ticks unless the
    // inactivity watchdog is also enabled.
    void SetTimeoutCheckInterval(std::chrono::milliseconds interval);

    // Peer endpoint of the connected socket, formatted as "host:port"
    // (bracketed IPv6 per RFC 3986). Cached on first access; empty for
    // AF_UNIX sockets or when getpeername(2) fails.
    std::string_view RemoteAddr();

    // Formats a sockaddr as "host:port" (bracketed for IPv6). Returns an
    // empty string for unsupported families (e.g. AF_UNIX) or null / zero-
    // length input. Exposed as a static so the format decision can be
    // unit-tested without a live socket.
    static std::string FormatSockAddr(const struct sockaddr* sa,
                                      socklen_t len);

    bool Disconnecting() const { return disconnecting_; }

    int Fd() const { return socket_.Get(); }
    iouring_runtime::core::SessionId GetSessionId() const { return session_id_; }

    // Trims `advanced` bytes off the front of (iovs, bufs). Fully consumed
    // iovecs (and their matching buffer refs, by parallel index) are erased
    // from both vectors; a partially consumed front iovec has its iov_base
    // advanced and its iov_len reduced. Required for re-issuing sendmsg
    // after a short io_uring write. Exposed as a static for unit testing.
    static void AdvanceSendState(std::vector<struct iovec>& iovs,
                                 std::vector<buffer::SendBufferRef>& bufs,
                                 std::size_t advanced);
    buffer::SendQueue::Stats SendQueueStats() const { return send_queue_.Snapshot(); }
    void SetSessionId(iouring_runtime::core::SessionId id) { session_id_ = id; }
    void SetConnectedCallback(ConnectedCallback cb) { on_connected_ = std::move(cb); }
    void SetDisconnectCallback(DisconnectCallback cb) { on_disconnect_ = std::move(cb); }
    void SetSendOverflowCallback(SendOverflowCallback cb) { on_send_overflow_ = std::move(cb); }
    void SetBackpressureCallback(BackpressureCallback cb) { on_backpressure_ = std::move(cb); }
    void SetBackpressureWatermarks(std::size_t high, std::size_t low) {
        backpressure_high_watermark_ = high;
        backpressure_low_watermark_ = low;
        if (backpressure_high_watermark_ != 0 &&
            backpressure_low_watermark_ > backpressure_high_watermark_) {
            backpressure_low_watermark_ = backpressure_high_watermark_;
        }
    }
    void SetDisconnectOnHighWatermark(bool enable) {
        disconnect_on_high_watermark_ = enable;
    }
    bool BackpressureActive() const { return backpressure_active_; }

protected:
    // Subclass implements packet-level processing.
    // Fast path: called with provided buffer data (zero-copy, in-place).
    // Slow path: called with RecvBuffer data after reassembly.
    virtual void OnRecv(std::span<const std::byte> data) = 0;
    virtual void OnConnected() {}
    virtual void OnDisconnected() {}
    virtual void OnSocketDrained() {}
    virtual bool HasPendingAppWork() const { return false; }
    virtual bool OnTimeoutTick(std::chrono::steady_clock::time_point /*now*/) {
        return false;
    }

    BufferPool& Pool() { return pool_; }
    IoRing& Ring() { return ring_; }
    void BeginAppIo();
    void EndAppIo();
    void MaybeDisconnectAfterFlush();
    bool HasPendingSocketWrites() const;
    void UpdateBackpressureState(std::size_t depth_hint = 0, bool hint_valid = false);

private:
    bool CanDisconnectAfterFlush() const;
    void RegisterRecv();
    void RegisterSend();
    void SendBatch(std::vector<SendBufferRef> bufs);
    void ArmWatchdog();
    void OnRecv(ring::RecvEvent& ev, std::int32_t result, std::uint32_t flags) override;
    void OnSend(ring::SendEvent& ev, std::int32_t result) override;
    void OnDisconnect(ring::DisconnectEvent& ev, std::int32_t result) override;
    void OnTimeout(ring::TimeoutEvent& ev, std::int32_t result) override;

    // Release self-ownership when all in-flight I/O has settled.
    void TryRelease();

    SocketHandle socket_;
    IoRing& ring_;
    BufferPool& pool_;
    buffer::SendQueue send_queue_;
    buffer::RecvBuffer recv_buf_;
    iouring_runtime::core::SessionId session_id_ = 0;
    ConnectedCallback on_connected_;
    DisconnectCallback on_disconnect_;
    SendOverflowCallback on_send_overflow_;
    BackpressureCallback on_backpressure_;
    bool disconnecting_ = false;
    bool disconnect_after_flush_ = false;
    bool backpressure_active_ = false;
    std::size_t backpressure_high_watermark_ = 0;
    std::size_t backpressure_low_watermark_ = 0;
    bool disconnect_on_high_watermark_ = false;

    // Counts in-flight io_uring ops whose CQEs reference Session members.
    // self_ref_ must not be released until this reaches zero.
    // Accessed from I/O thread only — no synchronization needed.
    int pending_io_ = 0;

    // Self-ownership: keeps Session alive during active I/O.
    // Released only via TryRelease() after all in-flight ops complete.
    std::shared_ptr<Session> self_ref_;

    // io_uring events — lifetime tied to Session
    ring::RecvEvent recv_ev_;
    ring::SendEvent send_ev_;
    ring::DisconnectEvent disconnect_ev_;
    ring::TimeoutEvent timeout_ev_;

    // Inactivity watchdog — non-zero timeout activates it. last_activity_
    // is refreshed on each successful recv; on each timeout tick the
    // elapsed idle time is compared against inactivity_timeout_.
    std::chrono::milliseconds inactivity_timeout_{0};
    std::chrono::milliseconds check_interval_{0};
    std::chrono::steady_clock::time_point last_activity_{};
    bool timeout_armed_ = false;

    // Cached peer endpoint string from getpeername(2). Resolved on first
    // RemoteAddr() access so sessions that never need it skip the syscall.
    std::string remote_addr_;
    bool remote_addr_resolved_ = false;

    // sendmsg state — must stay valid until CQE
    struct msghdr send_msg_{};
    std::vector<struct iovec> send_iovecs_;
    std::vector<SendBufferRef> in_flight_bufs_;
};

} // namespace iouring_runtime::core::io
