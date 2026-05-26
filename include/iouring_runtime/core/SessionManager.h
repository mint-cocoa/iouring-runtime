#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace iouring_runtime::core::io {

class Session;
using SessionRef = std::shared_ptr<Session>;

// Owns connected sessions. Pending io_uring events also hold strong session
// refs, so removing a session here does not destroy it until outstanding CQEs
// have released their event owners.
class SessionManager {
public:
    bool Add(SessionRef session);
    void Release(Session* session);
    void Release(const SessionRef& session);
    std::vector<SessionRef> Snapshot() const;
    std::size_t Count() const;
    void Clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<Session*, SessionRef> sessions_;
};

} // namespace iouring_runtime::core::io
