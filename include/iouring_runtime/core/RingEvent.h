#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <linux/time_types.h>  // struct __kernel_timespec (TimeoutEvent)
#include <liburing.h>

namespace iouring_runtime::core::ring {

enum class DispatchResult : std::uint8_t {
    kComplete,
    kPending,
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
    using CompletionFn =
        std::move_only_function<DispatchResult(IoEvent&, std::int32_t, std::uint32_t)>;

    explicit IoEvent(EventType type) noexcept : type_(type) {}
    virtual ~IoEvent() = default;

    EventType Type() const noexcept { return type_; }
    virtual DispatchResult Dispatch(std::int32_t result, std::uint32_t flags) {
        auto dispatch_owner = keep_alive_;
        if (completion_) {
            return completion_(*this, result, flags);
        }
        return DefaultDispatchResult(flags);
    }

    DispatchResult DefaultDispatchResult(std::uint32_t flags) const noexcept {
        switch (type_) {
            case EventType::kAccept:
            case EventType::kRecv:
                return MultishotDispatchResult(flags);
            default:
                return DispatchResult::kComplete;
        }
    }

    void SetCompletion(CompletionFn completion) {
        completion_ = std::move(completion);
    }

    void KeepAlive(std::shared_ptr<void> owner) {
        keep_alive_ = std::move(owner);
    }

    void ClearKeepAlive() noexcept {
        keep_alive_.reset();
    }

    bool AutoDelete() const noexcept { return auto_delete_; }
    void SetAutoDelete(bool value) noexcept { auto_delete_ = value; }
    bool ShouldDeleteAfterDispatch(DispatchResult result) const noexcept {
        return auto_delete_ && result == DispatchResult::kComplete;
    }

private:
    EventType type_;
    CompletionFn completion_;
    std::shared_ptr<void> keep_alive_;
    bool auto_delete_{false};
};

template <class EventT, class OwnerT, class HandlerEventT>
void BindCompletion(
    EventT& ev,
    std::shared_ptr<OwnerT> owner,
    DispatchResult (OwnerT::*handler)(HandlerEventT&, std::int32_t, std::uint32_t)) {
    static_assert(std::is_base_of_v<HandlerEventT, EventT>,
                  "handler event type must be a base of the bound event type");
    auto* raw_owner = owner.get();
    ev.KeepAlive(std::move(owner));
    ev.SetCompletion([raw_owner, handler](
                         IoEvent& ev,
                         std::int32_t result,
                         std::uint32_t flags) {
        return (raw_owner->*handler)(static_cast<HandlerEventT&>(ev), result, flags);
    });
}

template <class EventT, class OwnerT, class HandlerEventT>
void BindCompletion(
    EventT& ev,
    std::shared_ptr<OwnerT> owner,
    DispatchResult (OwnerT::*handler)(HandlerEventT&, std::int32_t)) {
    static_assert(std::is_base_of_v<HandlerEventT, EventT>,
                  "handler event type must be a base of the bound event type");
    auto* raw_owner = owner.get();
    ev.KeepAlive(std::move(owner));
    ev.SetCompletion([raw_owner, handler](
                         IoEvent& ev,
                         std::int32_t result,
                         std::uint32_t) {
        return (raw_owner->*handler)(static_cast<HandlerEventT&>(ev), result);
    });
}

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

} // namespace iouring_runtime::core::ring
