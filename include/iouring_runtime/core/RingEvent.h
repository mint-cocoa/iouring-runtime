#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <linux/time_types.h>  // struct __kernel_timespec (TimeoutEvent)

namespace iouring_runtime::core::ring {

// Forward declaration — EventHandler receives CQE callbacks via virtual dispatch
class EventHandler;
using EventHandlerRef = std::shared_ptr<EventHandler>;

// -- Event types ----

enum class EventType : std::uint8_t {
    kAccept,
    kRecv,
    kRead,
    kSend,
    kConnect,
    kDisconnect,
    kCancel,
    kPoll,
    kTimeout,
};

// Base event — stored as io_uring sqe user_data pointer.
// weak_ptr prevents CQE callbacks from keeping dead objects alive.
class IoEvent {
public:
    explicit IoEvent(EventType type) noexcept : type_(type) {}
    virtual ~IoEvent() = default;

    virtual void Init() { owner_.reset(); }

    EventType Type() const noexcept { return type_; }

    EventHandlerRef Owner() const { return owner_.lock(); }
    void SetOwner(const EventHandlerRef& o) { owner_ = o; }

private:
    EventType type_;
    std::weak_ptr<EventHandler> owner_;
};

// -- Concrete events ----

class AcceptEvent : public IoEvent {
public:
    AcceptEvent() : IoEvent(EventType::kAccept) {}
};

class RecvEvent : public IoEvent {
public:
    RecvEvent() : IoEvent(EventType::kRecv) {}

    void Init() override {
        IoEvent::Init();
        buffer_id_ = 0;
    }

    std::uint16_t BufferId() const noexcept { return buffer_id_; }
    void SetBufferId(std::uint16_t id) noexcept { buffer_id_ = id; }

private:
    std::uint16_t buffer_id_{0};
};

class ReadEvent : public IoEvent {
public:
    ReadEvent() : IoEvent(EventType::kRead) {}
};

class SendEvent : public IoEvent {
public:
    SendEvent() : IoEvent(EventType::kSend) {}

    void Init() override {
        IoEvent::Init();
        requested_bytes_ = 0;
    }

    std::size_t RequestedBytes() const noexcept { return requested_bytes_; }
    void SetRequestedBytes(std::size_t n) noexcept { requested_bytes_ = n; }

private:
    std::size_t requested_bytes_{0};
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
