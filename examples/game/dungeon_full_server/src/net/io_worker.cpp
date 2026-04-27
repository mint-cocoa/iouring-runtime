#include "io_worker.h"
#include <spdlog/spdlog.h>

using namespace iouring_runtime::core;
using namespace iouring_runtime::core::ring;
using namespace iouring_runtime::core::io;
using namespace iouring_runtime::core::job;

IoWorker::IoWorker(ContextId id, GlobalQueue& global_queue, JobTimer& timer)
    : id_(id)
    , global_queue_(global_queue)
    , timer_(timer)
{
}

void IoWorker::Start(const Address& addr, SessionFactory factory) {
    IoRingConfig cfg;
    cfg.queue_depth = 4096;
    cfg.buf_ring.buf_count = 8192;
    cfg.buf_ring.buf_size  = 8192;
    cfg.buf_ring.group_id  = static_cast<std::uint16_t>(id_ + 1);

    auto ring_result = IoRing::Create(cfg);
    if (!ring_result) {
        spdlog::error("IoWorker[{}]: failed to create IoRing", id_);
        return;
    }
    ring_ = std::move(*ring_result);

    listener_ = std::make_shared<Listener>(
        *ring_, pool_, addr, std::move(factory), id_, 0);

    auto listen_result = listener_->Start();
    if (!listen_result) {
        spdlog::error("IoWorker[{}]: failed to start listener on port {}", id_, addr.port);
        return;
    }

    running_ = true;
    thread_ = std::thread([this] { Run(); });
    spdlog::info("IoWorker[{}]: started on port {}", id_, addr.port);
}

void IoWorker::Stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

void IoWorker::Run() {
    IoRing::SetCurrent(ring_.get());
    while (running_) {
        ring_->Dispatch(std::chrono::milliseconds(1));
        ring_->ProcessPostedTasks();
        timer_.DistributeExpired();
        while (auto* q = global_queue_.TryPop())
            q->Execute(std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(8));
    }
}

void IoWorker::AddSession(SessionId sid, SessionRef session) {
    sessions_[sid] = std::move(session);
}

void IoWorker::RemoveSession(SessionId sid) {
    sessions_.erase(sid);
}

Session* IoWorker::FindSession(SessionId sid) {
    auto it = sessions_.find(sid);
    return it != sessions_.end() ? it->second.get() : nullptr;
}
