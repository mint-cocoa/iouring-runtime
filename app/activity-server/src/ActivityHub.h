#pragma once

#include "ActivityState.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace activity_server {

class ActivitySession;

class ActivityHub {
public:
    void Register(ActivitySession& session, std::shared_ptr<ActivitySession> self,
                  std::string instance_id, std::string client_id);
    void Remove(ActivitySession& session);
    void BroadcastText(std::string_view instance_id, const std::string& text);
    void BroadcastState(std::string_view instance_id,
                        std::optional<std::string> origin_client_id = std::nullopt);
    void BroadcastPresence(std::string_view instance_id);
    std::string QueueJson(std::string_view instance_id);
    std::string StatePayloadJson(std::string_view instance_id);
    std::string CreateDownload(std::string url, std::string instance_id,
                               bool force_play = false);
    std::string TaskJson(std::string_view task_id);

private:
    friend class ActivitySession;

    struct Client {
        std::weak_ptr<ActivitySession> session;
        std::string instance_id;
        std::string client_id;
    };

    void DownloadWorker(std::string task_id, std::string instance_id);
    InstanceState& StateLocked(std::string_view instance_id);
    std::vector<std::shared_ptr<ActivitySession>> SessionsLocked(std::string_view instance_id);
    std::string ClientsJsonLocked(std::string_view instance_id);
    std::string EntryJson(const QueueEntry& entry);

    std::mutex mu_;
    std::unordered_map<ActivitySession*, Client> clients_;
    std::unordered_map<std::string, InstanceState> states_;
    std::unordered_map<std::string, DownloadTask> tasks_;
};

} // namespace activity_server
