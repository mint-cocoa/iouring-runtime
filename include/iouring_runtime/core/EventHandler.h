#pragma once

#include <iouring_runtime/core/RingEvent.h>
#include <memory>

namespace iouring_runtime::core::ring {

class EventHandler : public std::enable_shared_from_this<EventHandler> {
public:
    virtual ~EventHandler() = default;

    void Dispatch(IoEvent* ev, std::int32_t result, std::uint32_t flags);

protected:
    virtual void OnAccept(AcceptEvent& ev, std::int32_t result, std::uint32_t flags) {}
    virtual void OnRecv(RecvEvent& ev, std::int32_t result, std::uint32_t flags) {}
    virtual void OnRead(ReadEvent& ev, std::int32_t result) {}
    virtual void OnSend(SendEvent& ev, std::int32_t result) {}
    virtual void OnConnect(ConnectEvent& ev, std::int32_t result) {}
    virtual void OnDisconnect(DisconnectEvent& ev, std::int32_t result) {}
    virtual void OnPoll(PollEvent& ev, std::int32_t result) {}
    virtual void OnTimeout(TimeoutEvent& ev, std::int32_t result) {}
};

inline void EventHandler::Dispatch(IoEvent* ev, std::int32_t result, std::uint32_t flags) {
    switch (ev->Type()) {
        case EventType::kAccept:
            OnAccept(static_cast<AcceptEvent&>(*ev), result, flags);
            break;
        case EventType::kRecv:
            OnRecv(static_cast<RecvEvent&>(*ev), result, flags);
            break;
        case EventType::kRead:
            OnRead(static_cast<ReadEvent&>(*ev), result);
            break;
        case EventType::kSend:
            OnSend(static_cast<SendEvent&>(*ev), result);
            break;
        case EventType::kConnect:
            OnConnect(static_cast<ConnectEvent&>(*ev), result);
            break;
        case EventType::kDisconnect:
            OnDisconnect(static_cast<DisconnectEvent&>(*ev), result);
            break;
        case EventType::kPoll:
            OnPoll(static_cast<PollEvent&>(*ev), result);
            break;
        case EventType::kTimeout:
            OnTimeout(static_cast<TimeoutEvent&>(*ev), result);
            break;
        case EventType::kCancel:
            break;
    }
}

using EventHandlerRef = std::shared_ptr<EventHandler>;

} // namespace iouring_runtime::core::ring
