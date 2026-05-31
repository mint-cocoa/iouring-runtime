#pragma once

#include <iouring_runtime/core/RingEvent.h>
#include <memory>

namespace iouring_runtime::core::ring {

class EventHandler : public std::enable_shared_from_this<EventHandler> {
public:
    virtual ~EventHandler() = default;

    DispatchResult Dispatch(IoEvent* ev, std::int32_t result, std::uint32_t flags);

protected:
    virtual DispatchResult OnAccept(AcceptEvent&, std::int32_t, std::uint32_t flags) {
        return MultishotResult(flags);
    }
    virtual DispatchResult OnRecv(RecvEvent&, std::int32_t, std::uint32_t flags) {
        return MultishotResult(flags);
    }
    virtual DispatchResult OnRead(ReadEvent&, std::int32_t) { return DispatchResult::kComplete; }
    virtual DispatchResult OnWrite(WriteEvent&, std::int32_t) { return DispatchResult::kComplete; }
    virtual DispatchResult OnSend(SendEvent&, std::int32_t) { return DispatchResult::kComplete; }
    virtual DispatchResult OnConnect(ConnectEvent&, std::int32_t) { return DispatchResult::kComplete; }
    virtual DispatchResult OnDisconnect(DisconnectEvent&, std::int32_t) { return DispatchResult::kComplete; }
    virtual DispatchResult OnCancel(CancelEvent&, std::int32_t) { return DispatchResult::kComplete; }
    virtual DispatchResult OnPoll(PollEvent&, std::int32_t) { return DispatchResult::kComplete; }
    virtual DispatchResult OnTimeout(TimeoutEvent&, std::int32_t) { return DispatchResult::kComplete; }

    static DispatchResult MultishotResult(std::uint32_t flags) noexcept {
        return MultishotDispatchResult(flags);
    }
};

inline DispatchResult EventHandler::Dispatch(IoEvent* ev, std::int32_t result, std::uint32_t flags) {
    switch (ev->Type()) {
        case EventType::kAccept:
            return OnAccept(static_cast<AcceptEvent&>(*ev), result, flags);
        case EventType::kRecv:
            return OnRecv(static_cast<RecvEvent&>(*ev), result, flags);
        case EventType::kRead:
            return OnRead(static_cast<ReadEvent&>(*ev), result);
        case EventType::kWrite:
            return OnWrite(static_cast<WriteEvent&>(*ev), result);
        case EventType::kSend:
            return OnSend(static_cast<SendEvent&>(*ev), result);
        case EventType::kConnect:
            return OnConnect(static_cast<ConnectEvent&>(*ev), result);
        case EventType::kDisconnect:
            return OnDisconnect(static_cast<DisconnectEvent&>(*ev), result);
        case EventType::kCancel:
            return OnCancel(static_cast<CancelEvent&>(*ev), result);
        case EventType::kPoll:
            return OnPoll(static_cast<PollEvent&>(*ev), result);
        case EventType::kTimeout:
            return OnTimeout(static_cast<TimeoutEvent&>(*ev), result);
    }
    return DispatchResult::kComplete;
}

using EventHandlerRef = std::shared_ptr<EventHandler>;

} // namespace iouring_runtime::core::ring
