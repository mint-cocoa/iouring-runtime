#pragma once

#include "../types.h"

#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Types.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

struct OutboundMessage {
    iouring_runtime::core::SessionId session_id = 0;
    RoomId room_id = 0;
    std::uint64_t room_seq = 0;
    std::move_only_function<void()> task;
    iouring_runtime::core::buffer::SendBufferRef buffer;
};

class WorkerOutbox {
public:
    void Enqueue(OutboundMessage msg);
    void DrainBudgeted(std::move_only_function<void(OutboundMessage)> sink,
                       std::size_t budget = 1024);
    std::size_t Pending() const;

private:
    using SessionId = iouring_runtime::core::SessionId;

    mutable std::mutex mutex_;
    std::deque<SessionId> ready_sessions_;
    std::unordered_set<SessionId> ready_set_;
    std::unordered_map<SessionId, std::deque<OutboundMessage>> queues_;

#ifndef NDEBUG
    std::unordered_map<SessionId, std::unordered_map<RoomId, std::uint64_t>>
        last_room_seq_;
#endif
};
