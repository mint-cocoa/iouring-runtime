#include "outbound_mailbox.h"

#include <cassert>
#include <optional>
#include <utility>

void WorkerOutbox::Enqueue(OutboundMessage msg) {
    if ((!msg.buffer && !msg.task) || msg.session_id == 0) {
        return;
    }

    std::lock_guard lock(mutex_);
    const auto sid = msg.session_id;
    queues_[sid].push_back(std::move(msg));

    if (!ready_set_.contains(sid)) {
        ready_set_.insert(sid);
        ready_sessions_.push_back(sid);
    }
}

void WorkerOutbox::DrainBudgeted(
    std::move_only_function<void(OutboundMessage)> sink, std::size_t budget) {
    for (std::size_t i = 0; i < budget; ++i) {
        std::optional<OutboundMessage> msg;

        {
            std::lock_guard lock(mutex_);
            if (ready_sessions_.empty()) {
                break;
            }

            const auto sid = ready_sessions_.front();
            ready_sessions_.pop_front();
            ready_set_.erase(sid);

            auto it = queues_.find(sid);
            if (it == queues_.end() || it->second.empty()) {
                continue;
            }

            msg = std::move(it->second.front());
            it->second.pop_front();

            if (!it->second.empty()) {
                ready_set_.insert(sid);
                ready_sessions_.push_back(sid);
            } else {
                queues_.erase(it);
            }
        }

#ifndef NDEBUG
        {
            std::lock_guard lock(mutex_);
            auto& last = last_room_seq_[msg->session_id][msg->room_id];
            assert(msg->room_seq > last);
            last = msg->room_seq;
        }
#endif

        sink(std::move(*msg));
    }
}

std::size_t WorkerOutbox::Pending() const {
    std::lock_guard lock(mutex_);
    std::size_t pending = 0;
    for (const auto& [_, queue] : queues_) {
        pending += queue.size();
    }
    return pending;
}
