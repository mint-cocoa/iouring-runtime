#include <iouring/net/Listener.h>

namespace iouring::net {

Listener::AcceptOp::AcceptOp(std::shared_ptr<Listener> owner)
    : owner_(std::move(owner)) {}

event::DispatchResult Listener::AcceptOp::Dispatch(std::int32_t result,
                                                   std::uint32_t flags) {
    return owner_->OnAccept(*this, result, flags);
}

Listener::CancelOp::CancelOp(std::shared_ptr<Listener> owner,
                             event::IoEvent* target)
    : event::CancelEvent(target)
    , owner_(std::move(owner)) {}

event::DispatchResult Listener::CancelOp::Dispatch(std::int32_t result,
                                                   std::uint32_t) {
    return owner_->OnCancel(*this, result);
}

} // namespace iouring::net
