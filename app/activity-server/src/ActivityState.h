#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace activity_server {

struct QueueEntry {
    std::string id;
    std::string url;
    std::string path;
    std::string source = "youtube";
    std::string title;
    std::string thumbnail;
    std::string ext = "m3u8";
    double duration = 0.0;
};

struct InstanceState {
    std::string current_video_url;
    QueueEntry metadata;
    std::vector<QueueEntry> queue;
    bool is_playing = false;
    double current_time = 0.0;
    std::chrono::steady_clock::time_point start_at{};
};

struct DownloadTask {
    std::string id;
    std::string url;
    std::string status = "pending";
    std::string error;
    QueueEntry entry;
    int progress = 0;
    bool force_play = false;
};

} // namespace activity_server
