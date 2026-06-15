#include <iouring/stream/AcmeHttpChallengeServer.h>

#include "AcmeChallengeSession.h"

#include <iouring/net/SessionControl.h>
#include <iouring/event/Worker.h>
#include <iouring/observability/Logging.h>

#include <memory>
#include <utility>

namespace obs = iouring::observability;

namespace {
constexpr auto kLogCategory = obs::LogCategory::kProxy;
}

namespace iouring::stream {

AcmeHttpChallengeServer::AcmeHttpChallengeServer(
    const AcmeHttpChallengeConfig& config)
    : config_(config) {}

AcmeHttpChallengeServer::~AcmeHttpChallengeServer() {
    Stop();
}

void AcmeHttpChallengeServer::Start() {
    if (running_ || !config_.Enabled()) {
        return;
    }
    running_ = true;

    for (std::uint16_t i = 0; i < config_.worker_count; ++i) {
        net::SessionFactory factory =
            [this](int fd, event::IoRing& ring,
                   core::buffer::BufferPool& pool, core::ContextId)
                -> net::SessionRef {
            auto session = detail::CreateAcmeChallengeSession(
                fd, ring, pool, config_.webroot,
                config_.send_queue_max_pending);
            net::SessionControl::SetInactivityTimeout(
                *session, config_.inactivity_timeout);
            return session;
        };

        event::WorkerConfig worker_config;
        worker_config.id = i;
        worker_config.address = core::Address{
            config_.listen_host, config_.listen_port};
        worker_config.ring.queue_depth = config_.ring.queue_depth;
        worker_config.ring.buf_ring.buf_count = config_.ring.buf_count;
        worker_config.ring.buf_ring.buf_size = config_.ring.buf_size;
        worker_config.ring.buf_ring.group_id = static_cast<std::uint16_t>(i + 1);
        worker_config.ring.submit_batch_size = config_.ring.submit_batch_size;
        worker_config.ring.cqe_batch_budget = config_.ring.cqe_batch_budget;
        worker_config.max_sessions = config_.max_sessions_per_worker;
        worker_config.io_timeout = config_.ring.io_timeout;
        worker_config.drain_timeout = config_.shutdown.drain_timeout;
        worker_config.force_close_timeout = config_.shutdown.force_close_timeout;

        auto worker = std::make_unique<event::Worker>(
            std::move(worker_config), std::move(factory));
        if (!worker->Start()) {
            obs::LogError(
                kLogCategory,
                "AcmeHttpChallengeServer: worker {} failed to listen on {}:{}",
                i, config_.listen_host, config_.listen_port);
            continue;
        }
        workers_.push_back(std::move(worker));
    }

    if (workers_.empty()) {
        running_ = false;
        obs::LogError(kLogCategory,
                      "AcmeHttpChallengeServer: failed to start listener");
        return;
    }

    obs::LogInfo(kLogCategory,
                 "AcmeHttpChallengeServer: listening on {}:{} ({} workers)",
                 config_.listen_host, config_.listen_port, workers_.size());
}

void AcmeHttpChallengeServer::Stop() {
    if (!running_ && workers_.empty()) {
        return;
    }
    running_ = false;
    for (auto& worker : workers_) {
        worker->Stop();
    }
    workers_.clear();
    obs::LogInfo(kLogCategory, "AcmeHttpChallengeServer: stopped");
}

} // namespace iouring::stream
