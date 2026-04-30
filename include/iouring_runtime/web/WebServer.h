#pragma once

#include <iouring_runtime/core/Worker.h>
#include <iouring_runtime/web/HttpMethod.h>
#include <iouring_runtime/web/HttpParser.h>
#include <iouring_runtime/web/HttpSession.h>
#include <iouring_runtime/web/Router.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iouring_runtime::web {

struct WebServerConfig {
    enum class WorkerAffinityMode {
        kOff,
        kPhysicalCores,
        kLogicalCpus,
    };

    struct RingOptions {
        std::uint32_t queue_depth = 2048;
        std::uint32_t buf_count = 4096;
        std::uint32_t buf_size = 4096;
        std::uint32_t submit_batch_size = 1;
        std::uint32_t cqe_batch_budget = 0;
        std::chrono::milliseconds io_timeout{1};
    };

    struct TimeoutOptions {
        std::chrono::milliseconds inactivity{30000};
        std::chrono::milliseconds request{0};
    };

    struct ShutdownOptions {
        std::chrono::milliseconds drain_timeout{1000};
        std::chrono::milliseconds force_close_timeout{200};
    };

    struct BackpressureOptions {
        std::uint32_t send_queue_max_pending = 4096;
        std::uint32_t send_queue_high_watermark = 0;
        std::uint32_t send_queue_low_watermark = 0;
        std::size_t send_queue_high_bytes = 0;
        std::size_t send_queue_low_bytes = 0;
        bool disconnect_on_high_watermark = false;
    };

    struct ObservabilityOptions {
        std::chrono::milliseconds slow_request_threshold{0};
    };

    std::string host = "0.0.0.0";
    std::uint16_t port = 8080;
    std::uint16_t worker_count = 4;
    WorkerAffinityMode worker_affinity = WorkerAffinityMode::kOff;
    std::uint32_t max_sessions_per_worker = 0;
    RingOptions ring;
    HttpParserOptions parser;
    TimeoutOptions timeouts;
    ShutdownOptions shutdown;
    BackpressureOptions backpressure;
    ObservabilityOptions observability;
};

class WebServer {
public:
    explicit WebServer(const WebServerConfig& config);
    ~WebServer();

    void Route(HttpMethod method, std::string path, HttpHandler handler);
    void RouteStream(HttpMethod method, std::string path,
                     HttpStreamHandler handler);
    void Use(HttpMiddleware middleware);

    void Get(std::string path, HttpHandler handler) {
        Route(HttpMethod::kGet, std::move(path), std::move(handler));
    }

    void Head(std::string path, HttpHandler handler) {
        Route(HttpMethod::kHead, std::move(path), std::move(handler));
    }

    void Post(std::string path, HttpHandler handler) {
        Route(HttpMethod::kPost, std::move(path), std::move(handler));
    }

    void Put(std::string path, HttpHandler handler) {
        Route(HttpMethod::kPut, std::move(path), std::move(handler));
    }

    void PutStream(std::string path, HttpStreamHandler handler) {
        RouteStream(HttpMethod::kPut, std::move(path), std::move(handler));
    }

    void PostStream(std::string path, HttpStreamHandler handler) {
        RouteStream(HttpMethod::kPost, std::move(path), std::move(handler));
    }

    void Delete(std::string path, HttpHandler handler) {
        Route(HttpMethod::kDelete, std::move(path), std::move(handler));
    }

    void Start();
    void Stop();

    static void InstallStopSignalHandlers();
    static void RequestStop() noexcept;
    static bool StopRequested() noexcept;
    static void WaitForStopSignal(
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds{100});
    static void ResetStopRequestedForTests() noexcept;

private:
    struct Worker {
        std::uint16_t index{0};
        int pinned_cpu{-1};
        std::atomic<core::SessionId> next_session_id{1};
        std::unique_ptr<core::io::Worker> io_worker;
    };

    void StopAccepting();
    void DrainSessions(bool force_close);
    bool WaitForZeroSessions(std::chrono::milliseconds timeout);
    void ConfigureWorkerAffinity(Worker& worker);
    void ConfigureWorkerThread(Worker& worker);

    WebServerConfig config_;
    Router router_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> running_{false};
};

} // namespace iouring_runtime::web
