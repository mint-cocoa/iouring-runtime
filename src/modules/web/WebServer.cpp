#include <iouring_runtime/web/WebServer.h>

#include <iouring_runtime/web/HttpSession.h>

#include <iouring_runtime/observability/Logging.h>

#include <cerrno>
#include <csignal>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kWeb;
}

namespace iouring_runtime::web {

namespace {

std::atomic<bool> g_stop_requested{false};

void WebServerSignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

const std::string& ServiceUnavailableResponse() {
    static const std::string response =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 19\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Service Unavailable";
    return response;
}

void SendBestEffortAndClose(int fd, std::string_view payload) {
    const char* current = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0) {
        const auto sent = ::send(fd, current, remaining, MSG_NOSIGNAL);
        if (sent > 0) {
            current += sent;
            remaining -= static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(fd);
}

} // namespace

WebServer::WebServer(const WebServerConfig& config)
    : config_(config) {}

WebServer::~WebServer() {
    Stop();
}

void WebServer::Route(HttpMethod method, std::string path, HttpHandler handler) {
    router_.Route(method, std::move(path), std::move(handler));
}

void WebServer::RouteStream(HttpMethod method, std::string path,
                            HttpStreamHandler handler) {
    router_.RouteStream(method, std::move(path), std::move(handler));
}

void WebServer::Use(HttpMiddleware middleware) {
    router_.Use(std::move(middleware));
}

void WebServer::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const Router* router = &router_;
    for (std::uint16_t i = 0; i < config_.worker_count; ++i) {
        auto worker = std::make_unique<Worker>();
        Worker* raw_worker = worker.get();
        worker->index = i;

        core::ring::IoRingConfig ring_config;
        ring_config.queue_depth = config_.ring.queue_depth;
        ring_config.buf_ring.buf_count = config_.ring.buf_count;
        ring_config.buf_ring.buf_size = config_.ring.buf_size;
        ring_config.buf_ring.group_id = static_cast<std::uint16_t>(i + 1);
        ring_config.submit_batch_size = config_.ring.submit_batch_size;
        ring_config.cqe_batch_budget = config_.ring.cqe_batch_budget;

        auto ring_result = core::ring::IoRing::Create(ring_config);
        if (!ring_result) {
            obs::LogError(kLogCategory, "WebServer: failed to create IoRing for worker {}", i);
            continue;
        }
        worker->ring = std::move(*ring_result);

        const auto parser_options = config_.parser;
        const auto timeouts = config_.timeouts;
        const auto backpressure = config_.backpressure;
        core::io::SessionFactory factory =
            [router, parser_options, timeouts, backpressure, raw_worker](
                int fd, core::ring::IoRing& ring,
                core::buffer::BufferPool& pool, core::ContextId)
                -> core::io::SessionRef {
            auto session = std::make_shared<HttpSession>(
                fd, ring, pool, *router, backpressure.send_queue_max_pending,
                parser_options, timeouts.request);
            session->SetSessionId(raw_worker->next_session_id.fetch_add(
                1, std::memory_order_relaxed));
            session->SetInactivityTimeout(timeouts.inactivity);
            session->SetBackpressureWatermarks(
                backpressure.send_queue_high_watermark,
                backpressure.send_queue_low_watermark);
            session->SetDisconnectOnHighWatermark(
                backpressure.disconnect_on_high_watermark);
            session->SetConnectedCallback([raw_worker](core::io::SessionRef session_ref) {
                raw_worker->live_sessions.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(raw_worker->sessions_mu);
                raw_worker->sessions.emplace(session_ref.get(), std::move(session_ref));
            });
            session->SetDisconnectCallback(
                [raw_worker](core::io::SessionRef session_ref) {
                    raw_worker->live_sessions.fetch_sub(1, std::memory_order_relaxed);
                    std::lock_guard lock(raw_worker->sessions_mu);
                    raw_worker->sessions.erase(session_ref.get());
                });
            return session;
        };

        core::Address addr{config_.host, config_.port};
        worker->listener = std::make_shared<core::io::Listener>(
            *worker->ring, worker->pool, addr, std::move(factory), i,
            config_.max_sessions_per_worker);
        worker->listener->SetSessionCountFn([raw_worker]() {
            return raw_worker->live_sessions.load(std::memory_order_relaxed);
        });
        worker->listener->SetRejectHandler([](int client_fd) {
            SendBestEffortAndClose(client_fd, ServiceUnavailableResponse());
        });

        auto listen_result = worker->listener->Start();
        if (!listen_result) {
            obs::LogError(kLogCategory, "WebServer: worker {} failed to listen on {}:{}",
                          i, config_.host, config_.port);
            continue;
        }

        worker->thread = std::thread([this, raw_worker]() {
            WorkerLoop(*raw_worker);
        });
        workers_.push_back(std::move(worker));
    }

    if (workers_.empty()) {
        running_.store(false, std::memory_order_release);
        obs::LogError(kLogCategory, "WebServer: failed to start any workers");
        return;
    }

    obs::LogInfo(kLogCategory, "WebServer: listening on {}:{} ({} workers)",
                 config_.host, config_.port, workers_.size());
}

void WebServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel) && workers_.empty()) {
        return;
    }
    if (workers_.empty()) {
        return;
    }

    StopAccepting();
    DrainSessions(false);
    if (!WaitForZeroSessions(config_.shutdown.drain_timeout)) {
        obs::LogWarn(kLogCategory,
            "WebServer: graceful drain timed out after {}ms, forcing close",
            config_.shutdown.drain_timeout.count());
        DrainSessions(true);
        WaitForZeroSessions(config_.shutdown.force_close_timeout);
    }

    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    workers_.clear();
    obs::LogInfo(kLogCategory, "WebServer: stopped");
}

void WebServer::InstallStopSignalHandlers() {
    std::signal(SIGINT, WebServerSignalHandler);
    std::signal(SIGTERM, WebServerSignalHandler);
}

void WebServer::RequestStop() noexcept {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

bool WebServer::StopRequested() noexcept {
    return g_stop_requested.load(std::memory_order_relaxed);
}

void WebServer::WaitForStopSignal(std::chrono::milliseconds poll_interval) {
    if (poll_interval <= std::chrono::milliseconds{0}) {
        poll_interval = std::chrono::milliseconds{100};
    }
    while (!StopRequested()) {
        std::this_thread::sleep_for(poll_interval);
    }
}

void WebServer::ResetStopRequestedForTests() noexcept {
    g_stop_requested.store(false, std::memory_order_relaxed);
}

} // namespace iouring_runtime::web
