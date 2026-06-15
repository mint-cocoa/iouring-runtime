#include <iouring/event/WorkerPool.h>

#include <iouring/observability/Logging.h>

#include <algorithm>
#include <utility>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kListener;
}

namespace iouring::event {

WorkerPool::WorkerPool(WorkerPoolConfig config,
                       WorkerSessionFactoryBuilder factory_builder,
                       WorkerHooks hooks)
    : config_(std::move(config))
    , factory_builder_(std::move(factory_builder))
    , hooks_(std::move(hooks)) {}

WorkerPool::~WorkerPool() {
    Stop();
}

bool WorkerPool::Start() {
    if (!workers_.empty()) {
        return true;
    }

    const auto count = std::max<std::uint16_t>(1, config_.worker_count);
    workers_.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        auto ring_config = config_.ring;
        ring_config.buf_ring.group_id = static_cast<std::uint16_t>(i + 1);

        WorkerConfig worker_config;
        worker_config.id = i;
        worker_config.address = config_.address;
        worker_config.ring = ring_config;
        worker_config.max_sessions = config_.max_sessions_per_worker;
        worker_config.io_timeout = config_.io_timeout;
        worker_config.drain_timeout = config_.drain_timeout;
        worker_config.force_close_timeout = config_.force_close_timeout;

        auto factory = factory_builder_(i);
        auto worker = std::make_unique<Worker>(
            std::move(worker_config), std::move(factory), hooks_);
        if (!worker->Start()) {
            obs::LogError(kLogCategory, "WorkerPool: worker {} failed to start", i);
            continue;
        }
        workers_.push_back(std::move(worker));
    }

    if (workers_.empty()) {
        return false;
    }
    return true;
}

void WorkerPool::Stop() {
    for (auto& worker : workers_) {
        worker->Stop();
    }
    workers_.clear();
}

Worker* WorkerPool::GetWorker(ContextId id) noexcept {
    if (id >= workers_.size()) {
        return nullptr;
    }
    return workers_[id].get();
}

const Worker* WorkerPool::GetWorker(ContextId id) const noexcept {
    if (id >= workers_.size()) {
        return nullptr;
    }
    return workers_[id].get();
}

} // namespace iouring::event
