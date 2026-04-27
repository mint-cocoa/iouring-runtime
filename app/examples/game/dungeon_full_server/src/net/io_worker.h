#pragma once

#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/JobQueue.h>
#include <iouring_runtime/core/JobTimer.h>
#include <iouring_runtime/core/GlobalQueue.h>
#include <iouring_runtime/core/Types.h>
#include <thread>
#include <atomic>
#include <unordered_map>

class IoWorker {
public:
    IoWorker(iouring_runtime::core::ContextId id,
             iouring_runtime::core::job::GlobalQueue& global_queue,
             iouring_runtime::core::job::JobTimer& timer);

    void Start(const iouring_runtime::core::Address& addr,
               iouring_runtime::core::io::SessionFactory factory);
    void Stop();

    iouring_runtime::core::ContextId             Id()   const { return id_; }
    iouring_runtime::core::ring::IoRing*         Ring()       { return ring_.get(); }
    iouring_runtime::core::buffer::BufferPool&   Pool()       { return pool_; }

    void AddSession(iouring_runtime::core::SessionId sid, iouring_runtime::core::io::SessionRef session);
    void RemoveSession(iouring_runtime::core::SessionId sid);
    iouring_runtime::core::io::Session* FindSession(iouring_runtime::core::SessionId sid);

private:
    void Run();

    iouring_runtime::core::ContextId id_;
    std::unique_ptr<iouring_runtime::core::ring::IoRing> ring_;
    iouring_runtime::core::buffer::BufferPool pool_;
    std::shared_ptr<iouring_runtime::core::io::Listener> listener_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::unordered_map<iouring_runtime::core::SessionId, iouring_runtime::core::io::SessionRef> sessions_;

    iouring_runtime::core::job::GlobalQueue& global_queue_;
    iouring_runtime::core::job::JobTimer& timer_;
};
