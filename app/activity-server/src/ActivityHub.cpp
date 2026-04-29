#include "ActivityHub.h"

#include "ActivitySession.h"
#include "ActivityUtils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

namespace activity_server {

InstanceState& ActivityHub::StateLocked(std::string_view instance_id) {
    return states_[std::string(instance_id.empty() ? "default" : instance_id)];
}

std::vector<std::shared_ptr<ActivitySession>>
ActivityHub::SessionsLocked(std::string_view instance_id) {
    std::vector<std::shared_ptr<ActivitySession>> sessions;
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (auto session = it->second.session.lock()) {
            if (it->second.instance_id == instance_id) {
                sessions.push_back(std::move(session));
            }
            ++it;
        } else {
            it = clients_.erase(it);
        }
    }
    return sessions;
}

std::string ActivityHub::ClientsJsonLocked(std::string_view instance_id) {
    std::string out = "[";
    bool first = true;
    for (const auto& [_, client] : clients_) {
        if (client.instance_id != instance_id) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"client_id\":\"" + JsonEscape(client.client_id) + "\"}";
    }
    out += "]";
    return out;
}

std::string ActivityHub::EntryJson(const QueueEntry& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << JsonEscape(entry.id)
        << "\",\"url\":\"" << JsonEscape(entry.url)
        << "\",\"path\":\"" << JsonEscape(entry.path)
        << "\",\"source\":\"" << JsonEscape(entry.source)
        << "\",\"title\":\"" << JsonEscape(entry.title)
        << "\",\"duration\":" << entry.duration
        << ",\"thumbnail\":\"" << JsonEscape(entry.thumbnail)
        << "\",\"ext\":\"" << JsonEscape(entry.ext) << "\"}";
    return out.str();
}

void ActivityHub::Register(ActivitySession& session, std::shared_ptr<ActivitySession> self,
                           std::string instance_id, std::string client_id) {
    std::lock_guard lock(mu_);
    clients_[&session] = Client{std::move(self), std::move(instance_id), std::move(client_id)};
}

void ActivityHub::Remove(ActivitySession& session) {
    std::string instance_id;
    {
        std::lock_guard lock(mu_);
        if (auto it = clients_.find(&session); it != clients_.end()) {
            instance_id = it->second.instance_id;
            clients_.erase(it);
        }
    }
    if (!instance_id.empty()) {
        BroadcastPresence(instance_id);
    }
}

void ActivityHub::BroadcastText(std::string_view instance_id, const std::string& text) {
    std::vector<std::shared_ptr<ActivitySession>> sessions;
    {
        std::lock_guard lock(mu_);
        sessions = SessionsLocked(instance_id);
    }
    for (auto& session : sessions) {
        session->SendTextAsync(text);
    }
}

std::string ActivityHub::StatePayloadJson(std::string_view instance_id) {
    std::lock_guard lock(mu_);
    auto& state = StateLocked(instance_id);
    const auto clients = ClientsJsonLocked(instance_id);
    double media_time = state.current_time;
    if (state.is_playing && state.start_at.time_since_epoch().count() != 0) {
        media_time += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - state.start_at).count();
    }
    std::string queue = "[";
    for (std::size_t i = 0; i < state.queue.size(); ++i) {
        if (i) queue += ',';
        queue += EntryJson(state.queue[i]);
    }
    queue += "]";

    std::ostringstream out;
    out << "{\"playback\":{\"is_playing\":" << (state.is_playing ? "true" : "false")
        << ",\"time\":" << std::fixed << std::setprecision(3) << media_time
        << ",\"paused_time\":" << state.current_time
        << ",\"server_now\":" << std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count()
        << "},\"media\":";
    if (state.current_video_url.empty()) {
        out << "null";
    } else {
        out << EntryJson(state.metadata);
    }
    out << ",\"queue\":" << queue
        << ",\"clients\":" << clients
        << ",\"client_count\":" << std::count_if(
               clients_.begin(), clients_.end(), [&](const auto& item) {
                   return item.second.instance_id == instance_id;
               })
        << "}";
    return out.str();
}

void ActivityHub::BroadcastState(std::string_view instance_id,
                                 std::optional<std::string> origin_client_id) {
    std::string text = "{\"type\":\"STATE_UPDATE\",\"seq\":1,\"origin\":";
    if (origin_client_id) {
        text += "{\"client_id\":\"" + JsonEscape(*origin_client_id) + "\"}";
    } else {
        text += "null";
    }
    text += ",\"payload\":" + StatePayloadJson(instance_id) + "}";
    BroadcastText(instance_id, text);
}

void ActivityHub::BroadcastPresence(std::string_view instance_id) {
    std::string payload;
    {
        std::lock_guard lock(mu_);
        const auto clients = ClientsJsonLocked(instance_id);
        const auto count = std::count_if(clients_.begin(), clients_.end(), [&](const auto& item) {
            return item.second.instance_id == instance_id;
        });
        payload = "{\"type\":\"PRESENCE_UPDATE\",\"payload\":{\"clients\":" +
                  clients + ",\"client_count\":" + std::to_string(count) + "}}";
    }
    BroadcastText(instance_id, payload);
}

std::string ActivityHub::QueueJson(std::string_view instance_id) {
    std::lock_guard lock(mu_);
    auto& state = StateLocked(instance_id);
    std::string out = "{\"queue\":[";
    for (std::size_t i = 0; i < state.queue.size(); ++i) {
        if (i) out += ',';
        out += EntryJson(state.queue[i]);
    }
    out += "]}";
    return out;
}

std::string ActivityHub::CreateDownload(std::string url, std::string instance_id,
                                        bool force_play) {
    const auto task_id = RandomId("dl_");
    {
        std::lock_guard lock(mu_);
        DownloadTask task;
        task.id = task_id;
        task.url = std::move(url);
        task.force_play = force_play;
        tasks_[task_id] = std::move(task);
    }
    std::thread([this, task_id, instance_id = std::move(instance_id)] {
        DownloadWorker(task_id, instance_id);
    }).detach();
    return task_id;
}

std::string ActivityHub::TaskJson(std::string_view task_id) {
    std::lock_guard lock(mu_);
    auto it = tasks_.find(std::string(task_id));
    if (it == tasks_.end()) return {};
    const auto& task = it->second;
    std::string out = "{\"task_id\":\"" + JsonEscape(task.id) +
        "\",\"status\":\"" + JsonEscape(task.status) +
        "\",\"url\":\"" + JsonEscape(task.url) +
        "\",\"progress\":" + std::to_string(task.progress);
    if (!task.error.empty()) {
        out += ",\"error\":\"" + JsonEscape(task.error) + "\"";
    }
    if (!task.entry.id.empty()) {
        out += ",\"entry\":" + EntryJson(task.entry);
    }
    out += "}";
    return out;
}

void ActivityHub::DownloadWorker(std::string task_id, std::string instance_id) {
    const auto downloads = std::filesystem::path(
        EnvString("ACTIVITY_DOWNLOAD_DIR", "/var/lib/iouring-runtime/activity-server/downloads"));
    const auto local_dir = downloads / "local" / "youtube";
    const auto hls_dir = downloads / "hls" / task_id;
    try {
        std::filesystem::create_directories(local_dir);
        std::filesystem::create_directories(hls_dir);
    } catch (const std::exception& ex) {
        {
            std::lock_guard lock(mu_);
            auto& task = tasks_[task_id];
            task.status = "failed";
            task.error = std::string("download directory error: ") + ex.what();
        }
        BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_FAILED\",\"task_id\":\"" +
            JsonEscape(task_id) + "\",\"error\":\"download directory error\"}");
        return;
    }

    std::string url;
    {
        std::lock_guard lock(mu_);
        auto& task = tasks_[task_id];
        task.status = "downloading";
        task.progress = 5;
        url = task.url;
    }
    BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_PROGRESS\",\"task_id\":\"" +
        JsonEscape(task_id) + "\",\"status\":\"downloading\",\"url\":\"" + JsonEscape(url) + "\"}");

    const auto output_template = (local_dir / (task_id + ".%(ext)s")).string();
    const auto media_file = (local_dir / (task_id + ".mp4")).string();
    const std::string ytdlp =
        "yt-dlp -f " +
        ShellQuote("bestvideo[vcodec^=avc1][ext=mp4][height<=1080]+bestaudio[ext=m4a]/best[ext=mp4][height<=1080]/best[height<=1080]") +
        " --merge-output-format mp4 --no-playlist -o " + ShellQuote(output_template) +
        " " + ShellQuote(url);

    if (RunCommand(ytdlp) != 0 || !std::filesystem::exists(media_file)) {
        {
            std::lock_guard lock(mu_);
            auto& task = tasks_[task_id];
            task.status = "failed";
            task.error = "yt-dlp failed";
        }
        BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_FAILED\",\"task_id\":\"" +
            JsonEscape(task_id) + "\",\"error\":\"yt-dlp failed\"}");
        return;
    }

    const auto playlist = hls_dir / "index.m3u8";
    const auto segment_pattern = hls_dir / "seg_%05d.ts";
    const auto ffmpeg_copy =
        "ffmpeg -y -i " + ShellQuote(media_file) +
        " -c copy -f hls -hls_time 6 -hls_playlist_type vod -hls_segment_filename " +
        ShellQuote(segment_pattern.string()) + " " + ShellQuote(playlist.string());
    const auto ffmpeg_transcode =
        "ffmpeg -y -i " + ShellQuote(media_file) +
        " -c:v libx264 -preset veryfast -c:a aac -f hls -hls_time 6 -hls_playlist_type vod -hls_segment_filename " +
        ShellQuote(segment_pattern.string()) + " " + ShellQuote(playlist.string());
    if ((RunCommand(ffmpeg_copy) != 0 || !std::filesystem::exists(playlist)) &&
        (RunCommand(ffmpeg_transcode) != 0 || !std::filesystem::exists(playlist))) {
        {
            std::lock_guard lock(mu_);
            auto& task = tasks_[task_id];
            task.status = "failed";
            task.error = "ffmpeg hls failed";
        }
        BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_FAILED\",\"task_id\":\"" +
            JsonEscape(task_id) + "\",\"error\":\"ffmpeg hls failed\"}");
        return;
    }

    QueueEntry entry;
    entry.id = task_id;
    entry.url = "/hls/" + task_id + "/index.m3u8";
    entry.path = playlist.string();
    entry.title = url;

    bool autostart = false;
    bool force_play = false;
    {
        std::lock_guard lock(mu_);
        auto& task = tasks_[task_id];
        task.status = "completed";
        task.progress = 100;
        task.entry = entry;
        force_play = task.force_play;

        auto& state = StateLocked(instance_id);
        if (force_play || (state.current_video_url.empty() && !state.is_playing)) {
            state.current_video_url = entry.url;
            state.metadata = entry;
            state.is_playing = true;
            state.current_time = 0.0;
            state.start_at = std::chrono::steady_clock::now();
            autostart = true;
        } else {
            state.queue.push_back(entry);
        }
    }

    BroadcastState(instance_id);
    BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_COMPLETE\",\"task_id\":\"" +
        JsonEscape(task_id) + "\",\"entry\":" + EntryJson(entry) +
        ",\"autostart\":" + (autostart ? "true" : "false") + "}");
}

} // namespace activity_server
