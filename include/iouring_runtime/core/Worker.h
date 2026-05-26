#pragma once

#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Types.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace iouring_runtime::core::io {

class Worker;

struct WorkerConfig {
    ContextId id{0};
    Address address{};
    ring::IoRingConfig ring{};
    std::uint32_t buffer_chunk_size{
        buffer::SendBufferChunk::kDefaultChunkSize};
    std::uint32_t buffer_max_chunks{256};
    std::uint32_t max_sessions{0};
    std::chrono::milliseconds io_timeout{1};
    std::chrono::milliseconds drain_timeout{1000};
    std::chrono::milliseconds force_close_timeout{200};
    std::function<void(int fd)> reject_handler;
    std::function<std::size_t()> extra_session_count;
};

struct WorkerHooks {
    std::function<void(Worker&)> on_start;
    std::function<void(Worker&)> tick;
};

class Worker {
public:
    Worker(WorkerConfig config, SessionFactory factory,
           WorkerHooks hooks = {});
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool Start();
    void Stop();

    void AddListener(Address address, SessionFactory factory,
                     std::uint32_t max_sessions = 0);
    void StopAccepting();
    void DrainSessions(bool force_close);
    bool WaitForZeroSessions(std::chrono::milliseconds timeout) const;

    void Post(std::move_only_function<void()> task);
    bool RunOnWorker(std::move_only_function<void()> task) noexcept;
    bool IsCurrentThread() const noexcept;

    ContextId Id() const noexcept { return config_.id; }
    std::size_t LiveSessions() const noexcept;
    ring::IoRing* Ring() noexcept { return ring_.get(); }
    buffer::BufferPool& Pool() noexcept { return pool_; }

private:
    void Run();
    std::shared_ptr<Listener> MakeListener(Address address,
                                           SessionFactory factory,
                                           std::uint32_t max_sessions);

    struct PendingListener {
        Address address;
        SessionFactory factory;
        std::uint32_t max_sessions{0};
    };

    WorkerConfig config_;
    SessionFactory factory_;
    WorkerHooks hooks_;

    std::unique_ptr<ring::IoRing> ring_;
    buffer::BufferPool pool_;
    std::shared_ptr<Listener> listener_;
    std::vector<PendingListener> pending_listeners_;
    std::vector<std::shared_ptr<Listener>> extra_listeners_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::thread::id thread_id_{};

};

} // namespace iouring_runtime::core::io
