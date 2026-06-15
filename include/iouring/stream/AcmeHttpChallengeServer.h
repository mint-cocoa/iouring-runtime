#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iouring::event {
class Worker;
} // namespace iouring::event

namespace iouring::stream {

struct AcmeHttpChallengeConfig {
    struct RingOptions {
        std::uint32_t queue_depth = 512;
        std::uint32_t buf_count = 1024;
        std::uint32_t buf_size = 4096;
        std::uint32_t submit_batch_size = 1;
        std::uint32_t cqe_batch_budget = 0;
        std::chrono::milliseconds io_timeout{1};
    };

    struct ShutdownOptions {
        std::chrono::milliseconds drain_timeout{1000};
        std::chrono::milliseconds force_close_timeout{200};
    };

    std::string listen_host = "0.0.0.0";
    std::uint16_t listen_port = 0;
    std::string webroot;
    std::uint16_t worker_count = 1;
    std::uint32_t max_sessions_per_worker = 0;
    std::uint32_t send_queue_max_pending = 16384;
    std::chrono::milliseconds inactivity_timeout{30000};
    RingOptions ring;
    ShutdownOptions shutdown;

    bool Enabled() const noexcept {
        return listen_port != 0 && !webroot.empty();
    }
};

class AcmeHttpChallengeServer {
public:
    explicit AcmeHttpChallengeServer(const AcmeHttpChallengeConfig& config);
    ~AcmeHttpChallengeServer();

    void Start();
    void Stop();

private:
    AcmeHttpChallengeConfig config_;
    std::vector<std::unique_ptr<iouring::event::Worker>> workers_;
    bool running_{false};
};

} // namespace iouring::stream
