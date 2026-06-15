#include <iouring/net/Session.h>

namespace iouring::net {

Session::RecvOp::RecvOp(SessionRef owner) : owner_(std::move(owner)) {}

event::DispatchResult Session::RecvOp::Dispatch(std::int32_t result,
                                                std::uint32_t flags) {
    return owner_->OnRecv(*this, result, flags);
}

Session::DisconnectOp::DisconnectOp(SessionRef owner)
    : owner_(std::move(owner)) {}

event::DispatchResult Session::DisconnectOp::Dispatch(std::int32_t result,
                                                      std::uint32_t) {
    return owner_->OnDisconnect(*this, result);
}

Session::CancelOp::CancelOp(SessionRef owner, event::IoEvent* target)
    : event::CancelEvent(target)
    , owner_(std::move(owner)) {}

event::DispatchResult Session::CancelOp::Dispatch(std::int32_t result,
                                                  std::uint32_t) {
    return owner_->OnCancel(*this, result);
}

Session::SendOp::SendOp(SessionRef owner) : owner_(std::move(owner)) {}

event::DispatchResult Session::SendOp::Dispatch(std::int32_t result,
                                                std::uint32_t) {
    return owner_->OnSend(*this, result);
}

} // namespace iouring::net
