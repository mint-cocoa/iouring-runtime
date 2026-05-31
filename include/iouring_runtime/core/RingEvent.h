#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <linux/time_types.h>  // struct __kernel_timespec (TimeoutEvent)
#include <liburing.h>

namespace iouring_runtime::core::ring {

class EventHandler;
using EventHandlerRef = std::shared_ptr<EventHandler>;

enum class DispatchResult : std::uint8_t {
    kComplete,
    kPending,
    kRearmed,
};

inline DispatchResult MultishotDispatchResult(std::uint32_t flags) noexcept {
    return (flags & IORING_CQE_F_MORE) != 0
        ? DispatchResult::kPending
        : DispatchResult::kComplete;
}

// -- Event types ----

enum class EventType : std::uint8_t {
    kAccept,
    kRecv,
    kRead,
    kWrite,
    kSend,
    kConnect,
    kDisconnect,
    kCancel,
    kPoll,
    kTimeout,
};

// Base operation context stored as io_uring SQE user_data and carried back by
// the CQE. Heap-allocated ops hold their owner alive until final CQE.
class IoEvent {
public:
    explicit IoEvent(EventType type) noexcept : type_(type) {}
    virtual ~IoEvent() = default;

    EventType Type() const noexcept { return type_; }
    DispatchResult DefaultDispatchResult(std::uint32_t flags) const noexcept {
        switch (type_) {
            case EventType::kAccept:
            case EventType::kRecv:
                return MultishotDispatchResult(flags);
            default:
                return DispatchResult::kComplete;
        }
    }

    EventHandlerRef Owner() const { return owner_ptr_; }
    void SetStrongOwner(EventHandlerRef owner_ptr) { owner_ptr_ = std::move(owner_ptr); }

    bool AutoDelete() const noexcept { return auto_delete_; }
    void SetAutoDelete(bool value) noexcept { auto_delete_ = value; }
    bool ShouldDeleteAfterDispatch(DispatchResult result) const noexcept {
        return auto_delete_ && result == DispatchResult::kComplete;
    }

private:
    EventType type_;
    EventHandlerRef owner_ptr_;
    bool auto_delete_{false};
};

// Convenience wrapper for constructing an event with a strong owner.
template <class EventT>
class StrongOwnedEvent : public EventT {
public:
    template <class... Args>
    explicit StrongOwnedEvent(EventHandlerRef owner_ptr, Args&&... args)
        : EventT(std::forward<Args>(args)...) {
        this->SetStrongOwner(std::move(owner_ptr));
    }
};

// -- Concrete events ----

class AcceptEvent : public IoEvent {
public:
    AcceptEvent() : IoEvent(EventType::kAccept) {}
};

class RecvEvent : public IoEvent {
public:
    RecvEvent() : IoEvent(EventType::kRecv) {}

    std::uint16_t BufferId() const noexcept { return buffer_id_; }
    void SetBufferId(std::uint16_t id) noexcept { buffer_id_ = id; }

private:
    std::uint16_t buffer_id_{0};
};

class ReadEvent : public IoEvent {
public:
    ReadEvent() : IoEvent(EventType::kRead) {}
};

class WriteEvent : public IoEvent {
public:
    WriteEvent() : IoEvent(EventType::kWrite) {}
};

class SendEvent : public IoEvent {
public:
    SendEvent() : IoEvent(EventType::kSend) {}

    std::size_t RequestedBytes() const noexcept { return requested_bytes_; }
    void SetRequestedBytes(std::size_t n) noexcept { requested_bytes_ = n; }

private:
    std::size_t requested_bytes_{0};
};

class CancelEvent : public IoEvent {
public:
    explicit CancelEvent(IoEvent* target = nullptr)
        : IoEvent(EventType::kCancel), target_(target) {}

    IoEvent* Target() const noexcept { return target_; }

private:
    IoEvent* target_;
};

class ConnectEvent : public IoEvent {
public:
    ConnectEvent() : IoEvent(EventType::kConnect) {}
};

class DisconnectEvent : public IoEvent {
public:
    DisconnectEvent() : IoEvent(EventType::kDisconnect) {}
};

class PollEvent : public IoEvent {
public:
    PollEvent() : IoEvent(EventType::kPoll) {}
};

// Carries storage for the kernel timespec backing an IORING_OP_TIMEOUT SQE.
// The `ts` field must outlive submission — storing it inline avoids a
// per-arm heap allocation. Same event instance can be reused after its CQE
// has been delivered.
class TimeoutEvent : public IoEvent {
public:
    TimeoutEvent() : IoEvent(EventType::kTimeout) {}
    struct __kernel_timespec ts{};
};

using StrongAcceptEvent = StrongOwnedEvent<AcceptEvent>;
using StrongCancelEvent = StrongOwnedEvent<CancelEvent>;
using StrongConnectEvent = StrongOwnedEvent<ConnectEvent>;
using StrongPollEvent = StrongOwnedEvent<PollEvent>;
using StrongTimeoutEvent = StrongOwnedEvent<TimeoutEvent>;
using StrongWriteEvent = StrongOwnedEvent<WriteEvent>;

} // namespace iouring_runtime::core::ring
