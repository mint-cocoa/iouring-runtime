#pragma once

#include <iouring_runtime/core/Noncopyable.h>
#include <iouring_runtime/core/Error.h>
#include <iouring_runtime/core/RingBuffer.h>
#include <iouring_runtime/core/RingEvent.h>
#include <iouring_runtime/core/SessionManager.h>
#include <iouring_runtime/observability/Profiler.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <sys/socket.h>
#include <sys/uio.h>

struct io_uring;
struct io_uring_sqe;

namespace iouring_runtime::core::ring {

struct IoRingConfig {
    std::size_t queue_depth{2048};
    RingBufferConfig buf_ring{};
    std::uint32_t submit_batch_size{1};   // 1 = submit immediately
    std::uint32_t cqe_batch_budget{0};    // 0 = drain all ready CQEs per dispatch

    bool single_issuer{false};           // IORING_SETUP_SINGLE_ISSUER
    bool sqpoll{false};                  // IORING_SETUP_SQPOLL
    std::uint32_t sq_thread_idle{1000};  // SQPOLL idle timeout (ms)
    int sq_thread_cpu{-1};               // SQPOLL CPU affinity (-1 = none)
};

// RAII wrapper around Linux io_uring.
//
// Provides:
//  - CQE dispatch loop (Dispatch)
//  - SQE preparation helpers (PrepRecv, PrepSend, PrepAccept, ...)
//  - Cross-thread task posting (RunOnRing, Post)
//  - Provided buffer ring management
class IoRing : Noncopyable {
public:
    static std::expected<std::unique_ptr<IoRing>, RingError> Create(const IoRingConfig& cfg = {});
    ~IoRing();

    // ── CQE dispatch ─────────────────────────────────────────
    bool Dispatch(std::chrono::milliseconds timeout);

    // ── SQE preparation ──────────────────────────────────────
    io_uring_sqe* GetSqe();
    int Submit();

    [[nodiscard]] bool PrepRecv(RecvEvent& ev, int fd);
    [[nodiscard]] bool PrepRecvMultishot(RecvEvent& ev, int fd);
    [[nodiscard]] bool PrepRead(ReadEvent& ev, int fd, void* buf, unsigned nbytes,
                                std::uint64_t offset);
    [[nodiscard]] bool PrepWrite(WriteEvent& ev, int fd, const void* buf,
                                 unsigned nbytes, std::uint64_t offset);
    [[nodiscard]] bool PrepSendMsg(SendEvent& ev, int fd, struct msghdr* msg, unsigned flags);
    [[nodiscard]] bool PrepAcceptMultishot(AcceptEvent& ev, int listen_fd);
    [[nodiscard]] bool PrepDisconnect(DisconnectEvent& ev, int fd);
    bool PrepCancel(IoEvent& target_ev, CancelEvent* cancel_ev = nullptr);
    [[nodiscard]] bool PrepConnect(ConnectEvent& ev, int fd, const struct sockaddr* addr, socklen_t len);
    [[nodiscard]] bool PrepPollAdd(PollEvent& ev, int fd, unsigned poll_mask);
    bool PrepPollRemove(PollEvent& ev);

    // Schedule an IORING_OP_TIMEOUT. The CQE arrives with res=-ETIME when
    // the duration elapses or res=-ECANCELED if PrepCancel(ev) is
    // subsequently submitted. The event's embedded __kernel_timespec is
    // populated from `duration`.
    [[nodiscard]] bool PrepTimeout(TimeoutEvent& ev, std::chrono::nanoseconds duration);

    // ── Cross-thread dispatch ────────────────────────────────
    bool RunOnRing(std::move_only_function<void()> task) noexcept;

    void Post(std::move_only_function<void()> task);
    void ProcessPostedTasks();

    // ── Accessors ────────────────────────────────────────────
    io_uring* Raw() const noexcept { return ring_; }
    int Fd() const noexcept;
    bool SupportsProvidedBuffers() const noexcept { return static_cast<bool>(buf_ring_); }
    RingBuffer& BufRing() noexcept { return *buf_ring_; }
    io::SessionManager& Sessions() noexcept { return session_manager_; }
    const io::SessionManager& Sessions() const noexcept { return session_manager_; }

    static IoRing* Current() noexcept;
    static void SetCurrent(IoRing* ring) noexcept;

private:
    struct Deleter {
        void operator()(io_uring* r) const noexcept;
    };

    explicit IoRing(io_uring* ring, std::unique_ptr<RingBuffer> br,
                    std::uint32_t submit_batch_size,
                    std::uint32_t cqe_batch_budget);
    int FlushSubmissions();
    void ArmWakePoll();
    void DrainWakeFd() noexcept;
    void Wake() noexcept;

    io_uring* ring_;
    std::unique_ptr<RingBuffer> buf_ring_;
    io::SessionManager session_manager_;
    std::uint32_t submit_batch_size_;
    std::uint32_t cqe_batch_budget_;
    std::uint32_t pending_submissions_ = 0;

    TracyLockable(std::mutex, post_mutex_);
    std::vector<std::move_only_function<void()>> posted_;
    int wake_fd_{-1};
    bool wake_poll_armed_{false};

    static thread_local IoRing* t_current_;
};

} // namespace iouring_runtime::core::ring
