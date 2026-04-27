#include "player_context.h"

PlayerContext* PlayerManager::Register(
    iouring_runtime::core::SessionId sid, PlayerId pid,
    iouring_runtime::core::ring::IoRing* ring,
    iouring_runtime::core::ContextId worker_id,
    std::weak_ptr<iouring_runtime::core::io::Session> sess)
{
    std::unique_lock lock(mutex_);
    auto ctx = std::make_unique<PlayerContext>();
    ctx->session_id = sid;
    ctx->player_id = pid;
    ctx->worker_ring = ring;
    ctx->worker_id = worker_id;
    ctx->session = std::move(sess);

    auto* ptr = ctx.get();
    by_session_[sid] = std::move(ctx);
    by_player_[pid] = ptr;
    return ptr;
}

void PlayerManager::Unregister(iouring_runtime::core::SessionId sid) {
    std::unique_lock lock(mutex_);
    auto it = by_session_.find(sid);
    if (it == by_session_.end()) return;

    auto* ctx = it->second.get();
    by_player_.erase(ctx->player_id);
    if (!ctx->char_name.empty())
        by_name_.erase(ctx->char_name);
    by_session_.erase(it);
}

PlayerContext* PlayerManager::FindBySession(iouring_runtime::core::SessionId sid) {
    std::shared_lock lock(mutex_);
    auto it = by_session_.find(sid);
    return it != by_session_.end() ? it->second.get() : nullptr;
}

PlayerContext* PlayerManager::FindByPlayer(PlayerId pid) {
    std::shared_lock lock(mutex_);
    auto it = by_player_.find(pid);
    return it != by_player_.end() ? it->second : nullptr;
}

PlayerContext* PlayerManager::FindByName(const std::string& name) {
    std::shared_lock lock(mutex_);
    auto it = by_name_.find(name);
    return it != by_name_.end() ? it->second : nullptr;
}
