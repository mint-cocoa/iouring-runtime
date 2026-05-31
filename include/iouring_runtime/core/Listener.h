#pragma once

#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/SocketHandle.h>
#include <iouring_runtime/core/Types.h>
#include <iouring_runtime/core/Error.h>
#include <iouring_runtime/core/EventHandler.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace iouring_runtime::core::io {

class Session;
using SessionRef = std::shared_ptr<Session>;
using ring::IoRing;

using SessionFactory = std::move_only_function<SessionRef(int fd, IoRing& ring,
                                                      buffer::BufferPool& pool, ContextId shard_id)>;

class Listener : public ring::EventHandler {
public:
    using SessionCountFn = std::function<std::size_t()>;
    using RejectHandler = std::move_only_function<void(int fd)>;

    Listener(IoRing& ring, buffer::BufferPool& pool, const Address& addr,
             SessionFactory factory, ContextId shard_id,
             std::uint32_t max_sessions = 0);

    std::expected<void, io::IoError> Start();
    void Stop();

    void SetSessionCountFn(SessionCountFn fn) { session_count_fn_ = std::move(fn); }
    void SetRejectHandler(RejectHandler handler) {
        reject_handler_ = std::move(handler);
    }

protected:
    ring::DispatchResult OnAccept(ring::AcceptEvent& ev, std::int32_t result, std::uint32_t flags) override;
    ring::DispatchResult OnCancel(ring::CancelEvent& ev, std::int32_t result) override;

private:
    bool RegisterAccept();
    void OnAccept(int client_fd);

    IoRing& ring_;
    buffer::BufferPool& pool_;
    Address addr_;
    SocketHandle listen_fd_;
    SessionFactory factory_;
    ring::AcceptEvent* active_accept_ev_ = nullptr;
    bool accept_cancel_requested_ = false;
    ContextId shard_id_;
    std::uint32_t max_sessions_;  // 0 = unlimited
    SessionCountFn session_count_fn_;
    RejectHandler reject_handler_;
};

} // namespace iouring_runtime::core::io
