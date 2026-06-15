#pragma once

#include <iouring/net/Session.h>
#include <iouring/core/Types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace iouring::net {

class SessionControl {
public:
    static void SetSessionId(Session& session, core::SessionId id) {
        session.session_id_ = id;
    }

    static void SetInactivityTimeout(Session& session,
                                     std::chrono::milliseconds timeout) {
        session.SetInactivityTimeout(timeout);
    }

    static void SetBackpressureWatermarks(Session& session,
                                          std::uint32_t high,
                                          std::uint32_t low) {
        session.send_queue_high_watermark_ = high;
        session.send_queue_low_watermark_ = low;
    }

    static void SetBackpressureByteWatermarks(Session& session,
                                              std::size_t high,
                                              std::size_t low) {
        session.send_queue_high_bytes_ = high;
        session.send_queue_low_bytes_ = low;
    }

    static void SetPauseRecvOnBackpressure(Session& session, bool enabled) {
        session.pause_recv_on_backpressure_ = enabled;
    }

    static void SetPausedRecvByteLimit(Session& session, std::size_t limit) {
        session.paused_recv_byte_limit_ = limit;
    }

    static void SetBackpressureDisconnectDelay(
        Session& session, std::chrono::milliseconds delay) {
        session.backpressure_disconnect_delay_ = delay;
    }

    static void SetDisconnectOnHighWatermark(Session& session, bool enabled) {
        session.disconnect_on_high_watermark_ = enabled;
    }
};

} // namespace iouring::net
