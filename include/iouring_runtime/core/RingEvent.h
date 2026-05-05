#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <linux/time_types.h>  // struct __kernel_timespec (TimeoutEvent)
#include <liburing.h>

namespace iouring_runtime::core::ring {

// Forward declaration — EventHandler receives CQE callbacks via virtual dispatch
class EventHandler;
using EventHandlerRef = std::shared_ptr<EventHandler>;

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

// Base operation context — stored as io_uring SQE user_data and carried back
// by the CQE. Heap-allocated ops hold their owner alive until final CQE.
class IoEvent {
public:
    explicit IoEvent(EventType type) noexcept : type_(type) {}
    virtual ~IoEvent() = default;

    virtual void Init() { strong_owner_.reset(); }
    virtual void Destroy() noexcept { delete this; }
    virtual bool Complete(std::int32_t /*result*/, std::uint32_t /*flags*/) const {
        return true;
    }

    EventType Type() const noexcept { return type_; }

    EventHandlerRef Owner() const { return strong_owner_; }
    void SetStrongOwner(EventHandlerRef o) { strong_owner_ = std::move(o); }

    bool AutoDelete() const noexcept { return auto_delete_; }
    void SetAutoDelete(bool value) noexcept { auto_delete_ = value; }
    void RetainAfterDispatch() noexcept { retain_after_dispatch_ = true; }
    bool ShouldDeleteAfterDispatch(std::int32_t result,
                                   std::uint32_t flags) noexcept {
        if (!auto_delete_) {
            return false;
        }
        if (retain_after_dispatch_) {
            retain_after_dispatch_ = false;
            return false;
        }
        return Complete(result, flags);
    }

private:
    EventType type_;
    EventHandlerRef strong_owner_;
    bool auto_delete_{false};
    bool retain_after_dispatch_{false};
};

// -- Concrete events ----

class AcceptEvent : public IoEvent {
public:
    AcceptEvent() : IoEvent(EventType::kAccept) {}
    bool Complete(std::int32_t /*result*/, std::uint32_t flags) const override;
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
    bool Complete(std::int32_t /*result*/, std::uint32_t flags) const override;

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

    void Init() override {
        IoEvent::Init();
        requested_bytes_ = 0;
    }

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

inline bool RecvEvent::Complete(std::int32_t /*result*/,
                                std::uint32_t flags) const {
    return (flags & IORING_CQE_F_MORE) == 0;
}

inline bool AcceptEvent::Complete(std::int32_t /*result*/,
                                  std::uint32_t flags) const {
    return (flags & IORING_CQE_F_MORE) == 0;
}

} // namespace iouring_runtime::core::ring
