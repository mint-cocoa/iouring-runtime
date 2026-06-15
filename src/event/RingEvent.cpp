#include <iouring/event/RingEvent.h>

namespace iouring::event {

DispatchResult MultishotDispatchResult(std::uint32_t flags) noexcept {
    return (flags & IORING_CQE_F_MORE) != 0
        ? DispatchResult::kPending
        : DispatchResult::kComplete;
}

IoEvent::IoEvent(EventType type) noexcept : type_(type) {}

IoEvent::~IoEvent() = default;

EventType IoEvent::Type() const noexcept {
    return type_;
}

DispatchResult IoEvent::Dispatch(std::int32_t result, std::uint32_t flags) {
    auto dispatch_owner = keep_alive_;
    if (completion_) {
        return completion_(*this, result, flags);
    }
    return DefaultDispatchResult(flags);
}

DispatchResult IoEvent::DefaultDispatchResult(std::uint32_t flags) const noexcept {
    switch (type_) {
    case EventType::kAccept:
    case EventType::kRecv:
        return MultishotDispatchResult(flags);
    default:
        return DispatchResult::kComplete;
    }
}

void IoEvent::SetCompletion(CompletionFn completion) {
    completion_ = std::move(completion);
}

void IoEvent::KeepAlive(std::shared_ptr<void> owner) {
    keep_alive_ = std::move(owner);
}

void IoEvent::ClearKeepAlive() noexcept {
    keep_alive_.reset();
}

bool IoEvent::AutoDelete() const noexcept {
    return auto_delete_;
}

void IoEvent::SetAutoDelete(bool value) noexcept {
    auto_delete_ = value;
}

bool IoEvent::ShouldDeleteAfterDispatch(DispatchResult result) const noexcept {
    return auto_delete_ && result == DispatchResult::kComplete;
}

AcceptEvent::AcceptEvent() : IoEvent(EventType::kAccept) {}

RecvEvent::RecvEvent() : IoEvent(EventType::kRecv) {}

std::uint16_t RecvEvent::BufferId() const noexcept {
    return buffer_id_;
}

void RecvEvent::SetBufferId(std::uint16_t id) noexcept {
    buffer_id_ = id;
}

ReadEvent::ReadEvent() : IoEvent(EventType::kRead) {}

WriteEvent::WriteEvent() : IoEvent(EventType::kWrite) {}

SendEvent::SendEvent() : IoEvent(EventType::kSend) {}

std::size_t SendEvent::RequestedBytes() const noexcept {
    return requested_bytes_;
}

void SendEvent::SetRequestedBytes(std::size_t n) noexcept {
    requested_bytes_ = n;
}

CancelEvent::CancelEvent(IoEvent* target)
    : IoEvent(EventType::kCancel)
    , target_(target) {}

IoEvent* CancelEvent::Target() const noexcept {
    return target_;
}

ConnectEvent::ConnectEvent() : IoEvent(EventType::kConnect) {}

DisconnectEvent::DisconnectEvent() : IoEvent(EventType::kDisconnect) {}

PollEvent::PollEvent() : IoEvent(EventType::kPoll) {}

TimeoutEvent::TimeoutEvent() : IoEvent(EventType::kTimeout) {}

} // namespace iouring::event
