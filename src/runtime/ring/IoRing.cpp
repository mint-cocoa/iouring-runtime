#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/observability/Profiler.h>

#include <liburing.h>
#include <iouring_runtime/observability/Logging.h>

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kRing;
constexpr std::uint64_t kWakeUserData = 0x2ULL;
}

namespace iouring_runtime::core::ring {

// -- TLS ----

thread_local IoRing* IoRing::t_current_ = nullptr;

IoRing* IoRing::Current() noexcept { return t_current_; }
void IoRing::SetCurrent(IoRing* ring) noexcept { t_current_ = ring; }

// -- Lifecycle ----

void IoRing::Deleter::operator()(io_uring* r) const noexcept {
    if (r) {
        io_uring_queue_exit(r);
        delete r;
    }
}

std::expected<std::unique_ptr<IoRing>, RingError> IoRing::Create(const IoRingConfig& cfg) {
    auto* raw = new (std::nothrow) io_uring{};
    if (!raw)
        return std::unexpected(RingError::kSetupFailed);

    io_uring_params params{};

    if (cfg.single_issuer)
        params.flags |= IORING_SETUP_SINGLE_ISSUER;

    if (cfg.sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = cfg.sq_thread_idle;

        if (cfg.sq_thread_cpu >= 0) {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = static_cast<unsigned>(cfg.sq_thread_cpu);
        }
    }

    int ret = io_uring_queue_init_params(static_cast<unsigned>(cfg.queue_depth), raw, &params);
    if (ret < 0) {
        // Graceful fallback: if SQPOLL fails (e.g. permission), retry without it
        if (cfg.sqpoll && ret == -EPERM) {
            obs::LogWarn(kLogCategory, "IoRing::Create: SQPOLL requires elevated privileges, falling back");
            params.flags &= ~(IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF);
            params.sq_thread_idle = 0;
            params.sq_thread_cpu = 0;
            ret = io_uring_queue_init_params(static_cast<unsigned>(cfg.queue_depth), raw, &params);
        }
        if (ret < 0) {
            delete raw;
            return std::unexpected(RingError::kSetupFailed);
        }
    }

    // Register ring fd for SQPOLL efficiency
    if (cfg.sqpoll && (params.flags & IORING_SETUP_SQPOLL))
        io_uring_register_ring_fd(raw);

    auto br_result = RingBuffer::Create(raw, cfg.buf_ring);
    std::unique_ptr<RingBuffer> buf_ring;
    if (br_result) {
        buf_ring = std::move(*br_result);
    } else {
        obs::LogWarn(kLogCategory,
                     "IoRing::Create: provided buffer ring unavailable; using compatibility recv path");
    }

    return std::unique_ptr<IoRing>(new IoRing(
        raw,
        std::move(buf_ring),
        std::max<std::uint32_t>(1, cfg.submit_batch_size),
        cfg.cqe_batch_budget));
}

IoRing::IoRing(io_uring* ring, std::unique_ptr<RingBuffer> br,
               std::uint32_t submit_batch_size,
               std::uint32_t cqe_batch_budget)
    : ring_(ring)
    , buf_ring_(std::move(br))
    , submit_batch_size_(submit_batch_size)
    , cqe_batch_budget_(cqe_batch_budget) {
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        obs::LogWarn(kLogCategory, "IoRing: eventfd wakeup disabled");
        return;
    }
    ArmWakePoll();
}

IoRing::~IoRing() {
    session_manager_.Clear();
    buf_ring_.reset();
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
    Deleter{}(ring_);
}

int IoRing::Fd() const noexcept {
    return ring_ ? ring_->ring_fd : -1;
}

// -- CQE dispatch ----

bool IoRing::Dispatch(std::chrono::milliseconds timeout) {
    ZoneScopedN("IoRing::Dispatch");
    FlushSubmissions();
    if (!wake_poll_armed_) {
        ArmWakePoll();
    }
    io_uring_cqe* cqe = nullptr;

    __kernel_timespec ts{};
    ts.tv_sec = static_cast<long long>(timeout.count() / 1000);
    ts.tv_nsec = static_cast<long long>((timeout.count() % 1000) * 1000000);
    int ret = 0;
    {
        ZoneScopedN("IoRing::Dispatch/wait_cqe");
        ret = io_uring_wait_cqe_timeout(ring_, &cqe, &ts);
    }

    if (ret < 0) {
        if (ret == -ETIME || ret == -EINTR)
            return true;
        obs::LogError(kLogCategory, "IoRing::Dispatch: wait_cqe failed: {}", ret);
        return false;
    }

    unsigned head = 0;
    unsigned count = 0;
    {
        ZoneScopedN("IoRing::Dispatch/drain_cqes");
        io_uring_for_each_cqe(ring_, head, cqe) {
            const std::uint64_t data = cqe->user_data;
            const std::int32_t result = cqe->res;
            const std::uint32_t flags = cqe->flags;

            if (data & 0x1ULL) {
                // MSG_RING task: bit 0 set
                auto* task = reinterpret_cast<std::move_only_function<void()>*>(data & ~0x1ULL);
                (*task)();
                delete task;
            } else if (data == kWakeUserData) {
                wake_poll_armed_ = false;
                if (result >= 0) {
                    DrainWakeFd();
                }
            } else if (data != 0) {
                auto* ev = reinterpret_cast<IoEvent*>(data);
                const auto dispatch_result = ev->Dispatch(result, flags);
                if (ev->ShouldDeleteAfterDispatch(dispatch_result)) {
                    delete ev;
                }
            }

            ++count;
            if (cqe_batch_budget_ != 0 && count >= cqe_batch_budget_) {
                break;
            }
        }
        io_uring_cq_advance(ring_, count);
    }

    if (!wake_poll_armed_) {
        ArmWakePoll();
    }

    return true;
}

// -- SQE helpers ----

io_uring_sqe* IoRing::GetSqe() {
    return io_uring_get_sqe(ring_);
}

int IoRing::Submit() {
    ++pending_submissions_;
    if (pending_submissions_ < submit_batch_size_) {
        return 0;
    }
    return FlushSubmissions();
}

int IoRing::FlushSubmissions() {
    if (pending_submissions_ == 0) {
        return 0;
    }
    ZoneScopedN("IoRing::FlushSubmissions");
    pending_submissions_ = 0;
    return io_uring_submit(ring_);
}

void IoRing::ArmWakePoll() {
    if (!ring_ || wake_fd_ < 0 || wake_poll_armed_) {
        return;
    }

    io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) {
        obs::LogWarn(kLogCategory, "IoRing::ArmWakePoll: SQE ring full");
        return;
    }

    io_uring_prep_poll_add(sqe, wake_fd_, POLLIN);
    io_uring_sqe_set_data64(sqe, kWakeUserData);
    if (io_uring_submit(ring_) < 0) {
        obs::LogWarn(kLogCategory, "IoRing::ArmWakePoll: submit failed");
        return;
    }
    wake_poll_armed_ = true;
}

void IoRing::DrainWakeFd() noexcept {
    if (wake_fd_ < 0) {
        return;
    }

    eventfd_t value = 0;
    while (::eventfd_read(wake_fd_, &value) == 0) {}
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        obs::LogWarn(kLogCategory, "IoRing::DrainWakeFd: read failed");
    }
}

void IoRing::Wake() noexcept {
    if (wake_fd_ < 0) {
        return;
    }

    if (::eventfd_write(wake_fd_, 1) != 0 &&
        errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        obs::LogWarn(kLogCategory, "IoRing::Wake: write failed");
    }
}

bool IoRing::PrepRecv(RecvEvent& ev, int fd) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepRecv: SQE ring full");
        return false;
    }
    if (buf_ring_) {
        io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = buf_ring_->GroupId();
    } else {
        ev.EnsureBuffer(8192);
        io_uring_prep_recv(sqe, fd, ev.MutableBufferData(), ev.BufferCapacity(), 0);
    }
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepRecvMultishot(RecvEvent& ev, int fd) {
    if (!buf_ring_) {
        return PrepRecv(ev, fd);
    }
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepRecvMultishot: SQE ring full");
        return false;
    }
    io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = buf_ring_->GroupId();
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepRead(ReadEvent& ev, int fd, void* buf, unsigned nbytes,
                      std::uint64_t offset) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepRead: SQE ring full");
        return false;
    }
    io_uring_prep_read(sqe, fd, buf, nbytes, static_cast<__u64>(offset));
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepWrite(WriteEvent& ev, int fd, const void* buf,
                       unsigned nbytes, std::uint64_t offset) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepWrite: SQE ring full");
        return false;
    }
    io_uring_prep_write(sqe, fd, buf, nbytes, static_cast<__u64>(offset));
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepSendMsg(SendEvent& ev, int fd,
                         struct msghdr* msg, unsigned flags) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepSendMsg: SQE ring full");
        return false;
    }
    // Calculate total bytes from iovec
    std::size_t total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; ++i)
        total += msg->msg_iov[i].iov_len;
    ev.SetRequestedBytes(total);
    io_uring_prep_sendmsg(sqe, fd, msg, flags);
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepAcceptMultishot(AcceptEvent& ev, int listen_fd) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepAcceptMultishot: SQE ring full");
        return false;
    }
    if (buf_ring_) {
        io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
    } else {
        io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    }
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepDisconnect(DisconnectEvent& ev, int fd) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepDisconnect: SQE ring full");
        return false;
    }
    io_uring_prep_shutdown(sqe, fd, SHUT_RDWR);
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepTimeout(TimeoutEvent& ev, std::chrono::nanoseconds duration) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepTimeout: SQE ring full");
        return false;
    }
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto rest = duration - std::chrono::duration_cast<std::chrono::nanoseconds>(secs);
    ev.ts.tv_sec = static_cast<long long>(secs.count());
    ev.ts.tv_nsec = static_cast<long long>(rest.count());
    io_uring_prep_timeout(sqe, &ev.ts, 0, 0);
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepCancel(IoEvent& target_ev, CancelEvent* cancel_ev) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        // Best-effort: if cancel SQE fails, shutdown will still terminate
        // the multishot recv by causing it to return EOF.
        obs::LogWarn(kLogCategory, "IoRing::PrepCancel: SQE ring full (shutdown will handle it)");
        return false;
    }
    // First arg: user_data of the target SQE to cancel.
    // If cancel_ev is null, the cancel CQE itself uses user_data=nullptr
    // so Dispatch ignores it. Callers that pass cancel_ev retain ownership
    // until the cancel CQE completes.
    io_uring_prep_cancel(sqe, &target_ev, 0);
    io_uring_sqe_set_data(sqe, cancel_ev);
    return true;
}

bool IoRing::PrepConnect(ConnectEvent& ev, int fd,
                         const struct sockaddr* addr, socklen_t len) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepConnect: SQE ring full");
        return false;
    }
    io_uring_prep_connect(sqe, fd, addr, len);
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepPollAdd(PollEvent& ev, int fd, unsigned poll_mask) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogError(kLogCategory, "IoRing::PrepPollAdd: SQE ring full");
        return false;
    }
    io_uring_prep_poll_add(sqe, fd, poll_mask);
    io_uring_sqe_set_data(sqe, &ev);
    return true;
}

bool IoRing::PrepPollRemove(PollEvent& ev) {
    io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        obs::LogWarn(kLogCategory, "IoRing::PrepPollRemove: SQE ring full");
        return false;
    }
    io_uring_prep_poll_remove(sqe, reinterpret_cast<__u64>(&ev));
    io_uring_sqe_set_data(sqe, nullptr);
    return true;
}

// -- Cross-thread dispatch ----

bool IoRing::RunOnRing(std::move_only_function<void()> task) noexcept {
    if (!ring_)
        return false;

    if (t_current_ == this) {
        task();
        return true;
    }

    if (t_current_ && t_current_->ring_) {
        auto* task_ptr = new (std::nothrow) std::move_only_function<void()>(std::move(task));
        if (!task_ptr)
            return false;

        std::uint64_t tagged = reinterpret_cast<std::uint64_t>(task_ptr) | 0x1ULL;

        io_uring_sqe* sqe = io_uring_get_sqe(t_current_->ring_);
        if (!sqe) {
            delete task_ptr;
            obs::LogWarn(kLogCategory, "IoRing::RunOnRing: source SQE ring full");
            return false;
        }

        io_uring_prep_msg_ring(sqe, Fd(), 0, tagged, 0);
        sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;

        if (io_uring_submit(t_current_->ring_) < 0) {
            delete task_ptr;
            obs::LogWarn(kLogCategory, "IoRing::RunOnRing: submit failed");
            return false;
        }

        return true;
    }

    Post(std::move(task));
    return true;
}

void IoRing::Post(std::move_only_function<void()> task) {
    {
        std::lock_guard lk(post_mutex_);
        LockMark(post_mutex_);
        posted_.push_back(std::move(task));
    }
    if (t_current_ != this) {
        Wake();
    }
}

void IoRing::ProcessPostedTasks() {
    ZoneScopedN("IoRing::ProcessPostedTasks");
    std::vector<std::move_only_function<void()>> tasks;
    {
        std::lock_guard lk(post_mutex_);
        LockMark(post_mutex_);
        tasks.swap(posted_);
    }
    for (auto& t : tasks)
        t();
}

} // namespace iouring_runtime::core::ring
