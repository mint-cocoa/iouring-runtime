#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace iouring::core {

struct ServerRingOptions {
    std::uint32_t queue_depth = 2048;
    std::uint32_t buf_count = 4096;
    std::uint32_t buf_size = 4096;
    std::uint32_t submit_batch_size = 1;
    std::uint32_t cqe_batch_budget = 0;
    std::chrono::milliseconds io_timeout{1};
};

struct InactivityTimeoutOptions {
    std::chrono::milliseconds inactivity{30000};
};

struct ServerShutdownOptions {
    std::chrono::milliseconds drain_timeout{1000};
    std::chrono::milliseconds force_close_timeout{200};
};

struct SendQueueBackpressureOptions {
    std::uint32_t send_queue_max_pending = 4096;
    std::uint32_t send_queue_high_watermark = 0;
    std::uint32_t send_queue_low_watermark = 0;
    std::size_t send_queue_high_bytes = 0;
    std::size_t send_queue_low_bytes = 0;
    bool disconnect_on_high_watermark = false;
    std::chrono::milliseconds disconnect_after{0};
    bool pause_recv_on_high_watermark = false;
    std::size_t paused_recv_byte_limit = 1024 * 1024;
};

} // namespace iouring::core
