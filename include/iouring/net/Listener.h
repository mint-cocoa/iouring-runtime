#pragma once

#include <iouring/event/IoRing.h>
#include <iouring/core/SendBuffer.h>
#include <iouring/net/SocketHandle.h>
#include <iouring/core/Types.h>
#include <iouring/core/Error.h>
#include <iouring/event/RingEvent.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace iouring::net {

class Session;
using SessionRef = std::shared_ptr<Session>;
using event::IoRing;
namespace buffer = iouring::core::buffer;
using core::Address;
using core::ContextId;

using SessionFactory = std::move_only_function<SessionRef(int fd, IoRing& ring,
                                                      buffer::BufferPool& pool, ContextId shard_id)>;

class Listener : public std::enable_shared_from_this<Listener> {
public:
    using SessionCountFn = std::function<std::size_t()>;
    using RejectHandler = std::move_only_function<void(int fd)>;

    Listener(IoRing& ring, buffer::BufferPool& pool, const Address& addr,
             SessionFactory factory, ContextId shard_id,
             std::uint32_t max_sessions = 0);

    std::expected<void, IoError> Start();
    void Stop();

    void SetSessionCountFn(SessionCountFn fn) { session_count_fn_ = std::move(fn); }
    void SetRejectHandler(RejectHandler handler) {
        reject_handler_ = std::move(handler);
    }

private:
    event::DispatchResult OnAccept(event::AcceptEvent& ev, std::int32_t result, std::uint32_t flags);
    event::DispatchResult OnCancel(event::CancelEvent& ev, std::int32_t result);
    bool RegisterAccept();
    void OnAccept(int client_fd);

    struct AcceptOp final : event::AcceptEvent {
        explicit AcceptOp(std::shared_ptr<Listener> owner);

        event::DispatchResult Dispatch(std::int32_t result, std::uint32_t flags) override;

    private:
        std::shared_ptr<Listener> owner_;
    };

    struct CancelOp final : event::CancelEvent {
        explicit CancelOp(std::shared_ptr<Listener> owner,
                          event::IoEvent* target = nullptr);

        event::DispatchResult Dispatch(std::int32_t result, std::uint32_t flags) override;

    private:
        std::shared_ptr<Listener> owner_;
    };

    IoRing& ring_;
    buffer::BufferPool& pool_;
    Address addr_;
    SocketHandle listen_fd_;
    SessionFactory factory_;
    AcceptOp* active_accept_ev_ = nullptr;
    bool accept_cancel_requested_ = false;
    ContextId shard_id_;
    std::uint32_t max_sessions_;  // 0 = unlimited
    SessionCountFn session_count_fn_;
    RejectHandler reject_handler_;
};

} // namespace iouring::net
