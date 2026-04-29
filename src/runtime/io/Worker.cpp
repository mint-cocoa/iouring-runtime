#include <iouring_runtime/core/Worker.h>

#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/observability/Profiler.h>

#include <chrono>
#include <utility>
#include <vector>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kListener;
}

namespace iouring_runtime::core::io {

Worker::Worker(WorkerConfig config, SessionFactory factory,
               WorkerHooks hooks)
    : config_(std::move(config))
    , factory_(std::move(factory))
    , hooks_(std::move(hooks))
    , pool_(config_.buffer_chunk_size, config_.buffer_max_chunks) {}

Worker::~Worker() {
    Stop();
}

bool Worker::Start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    auto ring_result = ring::IoRing::Create(config_.ring);
    if (!ring_result) {
        obs::LogError(kLogCategory, "Worker[{}]: failed to create IoRing",
                      config_.id);
        return false;
    }
    ring_ = std::move(*ring_result);

    listener_ = MakeListener(config_.address, std::move(factory_),
                             config_.max_sessions);

    auto listen_result = listener_->Start();
    if (!listen_result) {
        obs::LogError(kLogCategory, "Worker[{}]: failed to listen on {}:{}",
                      config_.id, config_.address.host, config_.address.port);
        listener_.reset();
        ring_.reset();
        return false;
    }

    for (auto& pending : pending_listeners_) {
        auto listener = MakeListener(
            std::move(pending.address), std::move(pending.factory),
            pending.max_sessions);
        auto extra_result = listener->Start();
        if (!extra_result) {
            obs::LogError(kLogCategory,
                          "Worker[{}]: failed to listen on {}:{}",
                          config_.id, pending.address.host,
                          pending.address.port);
            listener_->Stop();
            listener_.reset();
            extra_listeners_.clear();
            ring_.reset();
            return false;
        }
        extra_listeners_.push_back(std::move(listener));
    }
    pending_listeners_.clear();

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void Worker::Stop() {
    if (!ring_) {
        return;
    }

    if (running_.load(std::memory_order_acquire)) {
        StopAccepting();
        DrainSessions(false);
        if (!WaitForZeroSessions(config_.drain_timeout)) {
            DrainSessions(true);
            WaitForZeroSessions(config_.force_close_timeout);
        }

        running_.store(false, std::memory_order_release);
        ring_->Post([] {});
    }

    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }

    listener_.reset();
    extra_listeners_.clear();
    ring_.reset();
}

void Worker::AddListener(Address address, SessionFactory factory,
                         std::uint32_t max_sessions) {
    pending_listeners_.push_back(PendingListener{
        .address = std::move(address),
        .factory = std::move(factory),
        .max_sessions = max_sessions,
    });
}

void Worker::TrackSession(const SessionRef& session) {
    if (!session) {
        return;
    }

    session->AddConnectedCallback([this](SessionRef session_ref) {
        live_sessions_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(sessions_mu_);
        sessions_.emplace(session_ref.get(), std::move(session_ref));
    });
    session->AddDisconnectCallback([this](SessionRef session_ref) {
        live_sessions_.fetch_sub(1, std::memory_order_relaxed);
        std::lock_guard lock(sessions_mu_);
        sessions_.erase(session_ref.get());
    });
}

void Worker::StopAccepting() {
    if (!ring_) {
        return;
    }

    auto listener = listener_;
    auto extra_listeners = extra_listeners_;
    ring_->RunOnRing([listener = std::move(listener),
                      extra_listeners = std::move(extra_listeners)] {
        listener->Stop();
        for (auto& extra_listener : extra_listeners) {
            extra_listener->Stop();
        }
    });
}

void Worker::DrainSessions(bool force_close) {
    if (!ring_) {
        return;
    }

    std::vector<SessionRef> sessions;
    {
        std::lock_guard lock(sessions_mu_);
        sessions.reserve(sessions_.size());
        for (const auto& [_, session] : sessions_) {
            sessions.push_back(session);
        }
    }

    if (sessions.empty()) {
        return;
    }

    ring_->RunOnRing([sessions = std::move(sessions), force_close]() mutable {
        for (auto& session : sessions) {
            if (!session || session->Disconnecting()) {
                continue;
            }
            if (force_close) {
                session->Disconnect();
            } else {
                session->DisconnectAfterFlush();
            }
        }
    });
}

bool Worker::WaitForZeroSessions(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (live_sessions_.load(std::memory_order_relaxed) == 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

void Worker::Post(std::move_only_function<void()> task) {
    if (ring_) {
        ring_->Post(std::move(task));
    }
}

bool Worker::RunOnWorker(std::move_only_function<void()> task) noexcept {
    if (!ring_) {
        return false;
    }
    return ring_->RunOnRing(std::move(task));
}

bool Worker::IsCurrentThread() const noexcept {
    return std::this_thread::get_id() == thread_id_;
}

std::size_t Worker::LiveSessions() const noexcept {
    return live_sessions_.load(std::memory_order_relaxed);
}

void Worker::Run() {
    thread_id_ = std::this_thread::get_id();
    ring::IoRing::SetCurrent(ring_.get());
    if (hooks_.on_start) {
        hooks_.on_start(*this);
    }

    while (running_.load(std::memory_order_relaxed)) {
        ring_->ProcessPostedTasks();
        ring_->Dispatch(config_.io_timeout);
        ring_->ProcessPostedTasks();
        if (hooks_.tick) {
            hooks_.tick(*this);
        }
    }

    ring_->ProcessPostedTasks();
    for (int i = 0; i < 8; ++i) {
        ring_->Dispatch(std::chrono::milliseconds{0});
        ring_->ProcessPostedTasks();
    }
    ring::IoRing::SetCurrent(nullptr);
}

std::shared_ptr<Listener> Worker::MakeListener(
    Address address, SessionFactory factory, std::uint32_t max_sessions) {
    SessionFactory wrapped_factory =
        [this, factory = std::move(factory)](
            int fd, ring::IoRing& ring_ref, buffer::BufferPool& pool_ref,
            ContextId shard_id) mutable -> SessionRef {
        auto session = factory(fd, ring_ref, pool_ref, shard_id);
        if (!session) {
            return nullptr;
        }
        TrackSession(session);
        return session;
    };

    auto listener = std::make_shared<Listener>(
        *ring_, pool_, address, std::move(wrapped_factory),
        config_.id, max_sessions);
    listener->SetSessionCountFn([this] {
        auto count = live_sessions_.load(std::memory_order_relaxed);
        if (config_.extra_session_count) {
            count += config_.extra_session_count();
        }
        return count;
    });
    if (config_.reject_handler) {
        listener->SetRejectHandler([handler = config_.reject_handler](int fd) {
            handler(fd);
        });
    }
    return listener;
}

} // namespace iouring_runtime::core::io
