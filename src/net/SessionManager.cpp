#include <iouring/net/SessionManager.h>

#include <iouring/net/Session.h>

namespace iouring::net {

bool SessionManager::Add(SessionRef session) {
    if (!session) {
        return false;
    }

    auto* key = session.get();
    std::lock_guard lock(mutex_);
    return sessions_.emplace(key, std::move(session)).second;
}

void SessionManager::Release(Session* session) {
    if (!session) {
        return;
    }

    std::lock_guard lock(mutex_);
    sessions_.erase(session);
}

void SessionManager::Release(const SessionRef& session) {
    Release(session.get());
}

std::vector<SessionRef> SessionManager::Snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<SessionRef> sessions;
    sessions.reserve(sessions_.size());
    for (const auto& [_, session] : sessions_) {
        sessions.push_back(session);
    }
    return sessions;
}

std::size_t SessionManager::Count() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

void SessionManager::Clear() {
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

} // namespace iouring::net
