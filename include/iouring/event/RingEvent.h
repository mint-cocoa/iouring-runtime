#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <linux/time_types.h>  // struct __kernel_timespec (TimeoutEvent)
#include <liburing.h>

namespace iouring::event {

enum class DispatchResult : std::uint8_t {
    kComplete,
    kPending,
};

DispatchResult MultishotDispatchResult(std::uint32_t flags) noexcept;

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

    explicit IoEvent(EventType type) noexcept;
    virtual ~IoEvent();

    EventType Type() const noexcept;
    virtual DispatchResult Dispatch(std::int32_t result, std::uint32_t flags);

    DispatchResult DefaultDispatchResult(std::uint32_t flags) const noexcept;

    void SetCompletion(CompletionFn completion);

    void KeepAlive(std::shared_ptr<void> owner);

    void ClearKeepAlive() noexcept;

    bool AutoDelete() const noexcept;
    void SetAutoDelete(bool value) noexcept;
    bool ShouldDeleteAfterDispatch(DispatchResult result) const noexcept;

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
    AcceptEvent();
};

class RecvEvent : public IoEvent {
public:
    RecvEvent();

    std::uint16_t BufferId() const noexcept;
    void SetBufferId(std::uint16_t id) noexcept;

private:
    std::uint16_t buffer_id_{0};
};

class ReadEvent : public IoEvent {
public:
    ReadEvent();
};

class WriteEvent : public IoEvent {
public:
    WriteEvent();
};

class SendEvent : public IoEvent {
public:
    SendEvent();

    std::size_t RequestedBytes() const noexcept;
    void SetRequestedBytes(std::size_t n) noexcept;

private:
    std::size_t requested_bytes_{0};
};

class CancelEvent : public IoEvent {
public:
    explicit CancelEvent(IoEvent* target = nullptr);

    IoEvent* Target() const noexcept;

private:
    IoEvent* target_;
};

class ConnectEvent : public IoEvent {
public:
    ConnectEvent();
};

class DisconnectEvent : public IoEvent {
public:
    DisconnectEvent();
};

class PollEvent : public IoEvent {
public:
    PollEvent();
};

// Carries storage for the kernel timespec backing an IORING_OP_TIMEOUT SQE.
// The `ts` field must outlive submission — storing it inline avoids a
// per-arm heap allocation. Same event instance can be reused after its CQE
// has been delivered.
class TimeoutEvent : public IoEvent {
public:
    TimeoutEvent();
    struct __kernel_timespec ts{};
};

} // namespace iouring::event
