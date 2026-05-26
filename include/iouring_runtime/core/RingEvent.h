#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <linux/time_types.h>  // struct __kernel_timespec (TimeoutEvent)
#include <liburing.h>

namespace iouring_runtime::core::ring {

class EventHandler;
using EventHandlerRef = std::shared_ptr<EventHandler>;

class DrainGate {
public:
    class Token {
    public:
        Token() = default;
        explicit Token(DrainGate& gate) noexcept : gate_(&gate) {
            ++gate_->count_;
        }

        Token(Token&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)) {}

        Token& operator=(Token&& other) noexcept {
            if (this != &other) {
                Reset();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }

        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;

        ~Token() {
            Reset();
        }

        void Reset() noexcept {
            if (!gate_) {
                return;
            }
            auto* gate = std::exchange(gate_, nullptr);
            --gate->count_;
            gate->OnLeave();
        }

        explicit operator bool() const noexcept { return gate_ != nullptr; }

    private:
        DrainGate* gate_ = nullptr;
    };

    Token Enter() noexcept { return Token(*this); }

    void SetOnDrained(std::move_only_function<void()> on_drained) {
        on_drained_ = std::move(on_drained);
    }

    void Close() {
        closing_ = true;
        OnLeave();
    }

    [[nodiscard]] bool Drained() const noexcept { return count_ == 0; }
    [[nodiscard]] int Count() const noexcept { return count_; }

private:
    void OnLeave() {
        if (closing_ && count_ == 0 && on_drained_) {
            on_drained_();
        }
    }

    int count_ = 0;
    bool closing_ = false;
    std::move_only_function<void()> on_drained_;
};

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

    virtual bool Complete(std::int32_t /*result*/, std::uint32_t /*flags*/) const {
        return true;
    }

    EventType Type() const noexcept { return type_; }

    EventHandlerRef Owner() const { return owner_ptr_; }
    void SetStrongOwner(EventHandlerRef owner_ptr) { owner_ptr_ = std::move(owner_ptr); }
    void SetDrainToken(DrainGate::Token token) { drain_token_ = std::move(token); }

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
    EventHandlerRef owner_ptr_;
    DrainGate::Token drain_token_;
    bool auto_delete_{false};
    bool retain_after_dispatch_{false};
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
    bool Complete(std::int32_t /*result*/, std::uint32_t flags) const override;
};

class RecvEvent : public IoEvent {
public:
    RecvEvent() : IoEvent(EventType::kRecv) {}

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

inline bool RecvEvent::Complete(std::int32_t /*result*/,
                                std::uint32_t flags) const {
    return (flags & IORING_CQE_F_MORE) == 0;
}

inline bool AcceptEvent::Complete(std::int32_t /*result*/,
                                  std::uint32_t flags) const {
    return (flags & IORING_CQE_F_MORE) == 0;
}

} // namespace iouring_runtime::core::ring
