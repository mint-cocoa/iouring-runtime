#include <iouring_runtime/core/EventHandler.h>
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/web/WebServer.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using iouring_runtime::web::HttpMethod;
using iouring_runtime::web::HttpStatus;
using iouring_runtime::web::HttpStreamHandler;
using iouring_runtime::web::RequestContext;
using iouring_runtime::web::DeferredResponse;

namespace {

constexpr std::uint64_t kDefaultMaxUploadBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kDefaultSendChunkBytes = 256U * 1024U;

struct UploadState {
    struct PendingWrite : iouring_runtime::core::ring::WriteEvent {
        std::vector<std::byte> buffer;
        std::size_t written = 0;
        std::uint64_t offset = 0;
    };

    int fd = -1;
    std::filesystem::path temp_path;
    std::filesystem::path final_path;
    std::uint64_t expected_bytes = 0;
    std::uint64_t submitted_bytes = 0;
    std::uint64_t written_bytes = 0;
    bool failed = false;
    bool aborted = false;
    bool request_complete = false;
    bool finished = false;
    std::string error;
};

class AsyncUploadState
    : public iouring_runtime::core::ring::EventHandler {
public:
    using PendingWrite = UploadState::PendingWrite;

    ~AsyncUploadState() override {
        Close();
    }

    void Start(RequestContext& /*ctx*/, UploadState upload,
               std::function<void()> cleanup) {
        ring_ = iouring_runtime::core::ring::IoRing::Current();
        upload_ = std::move(upload);
        cleanup_ = std::move(cleanup);
    }

    bool SubmitChunk(std::span<const std::byte> chunk) {
        if (upload_.failed || upload_.finished) {
            return true;
        }
        auto op = std::make_unique<PendingWrite>();
        op->buffer.assign(chunk.begin(), chunk.end());
        op->offset = upload_.submitted_bytes;
        upload_.submitted_bytes += chunk.size();
        auto* event = op.release();
        event->SetStrongOwner(shared_from_this());
        event->SetAutoDelete(true);
        pending_.emplace(event, event);
        if (!SubmitPendingWrite(*event)) {
            pending_.erase(event);
            delete event;
            TryFinish();
        }
        return true;
    }

    void MarkRequestComplete(DeferredResponse deferred) {
        deferred_ = std::move(deferred);
        upload_.request_complete = true;
        TryFinish();
    }

    void Abort() {
        if (upload_.finished || upload_.aborted) {
            return;
        }
        upload_.aborted = true;
        Close();
        std::filesystem::remove(upload_.temp_path);
        if (deferred_) {
            deferred_->Abort();
        }
        if (pending_.empty()) {
            upload_.finished = true;
            Cleanup();
        }
    }

protected:
    void OnWrite(iouring_runtime::core::ring::WriteEvent& ev,
                 std::int32_t result) override {
        auto it = pending_.find(&ev);
        if (it == pending_.end()) {
            return;
        }

        if (upload_.aborted) {
            pending_.erase(it);
            TryFinish();
            return;
        }

        auto& op = *it->second;
        if (result <= 0) {
            upload_.failed = true;
            upload_.error = "async write failed";
            pending_.erase(it);
            TryFinish();
            return;
        }

        op.written += static_cast<std::size_t>(result);
        if (op.written < op.buffer.size()) {
            if (!SubmitPendingWrite(op)) {
                pending_.erase(it);
                TryFinish();
                return;
            }
            op.RetainAfterDispatch();
            return;
        }

        upload_.written_bytes += op.buffer.size();
        pending_.erase(it);
        TryFinish();
    }

private:
    bool SubmitPendingWrite(PendingWrite& op) {
        if (!ring_ || upload_.fd < 0) {
            upload_.failed = true;
            upload_.error = "io_uring unavailable";
            return false;
        }
        const auto remaining = op.buffer.size() - op.written;
        const auto* data = op.buffer.data() + op.written;
        if (!ring_->PrepWrite(op, upload_.fd, data,
                              static_cast<unsigned>(remaining),
                              op.offset + op.written)) {
            upload_.failed = true;
            upload_.error = "submit write failed";
            return false;
        }
        ring_->Submit();
        return true;
    }

    void TryFinish() {
        if (upload_.finished) {
            return;
        }
        if (upload_.aborted) {
            if (pending_.empty()) {
                upload_.finished = true;
                Cleanup();
            }
            return;
        }
        if (!upload_.request_complete || !deferred_ || !pending_.empty()) {
            return;
        }
        upload_.finished = true;
        Close();

        if (upload_.failed ||
            (upload_.expected_bytes != 0 &&
             upload_.submitted_bytes != upload_.expected_bytes) ||
            upload_.written_bytes != upload_.submitted_bytes) {
            std::filesystem::remove(upload_.temp_path);
            SendResponse(HttpStatus::kInternalServerError,
                         upload_.failed ? upload_.error : "short upload");
            FinishDeferredResponse();
            return;
        }

        std::error_code ec;
        std::filesystem::rename(upload_.temp_path, upload_.final_path, ec);
        if (ec) {
            std::filesystem::remove(upload_.temp_path);
            SendResponse(HttpStatus::kInternalServerError, "rename failed");
            FinishDeferredResponse();
            return;
        }

        deferred_->Response().Status(HttpStatus::kCreated)
            .KeepAlive(false)
            .Json("{\"ok\":true,\"bytes\":" +
                  std::to_string(upload_.written_bytes) + "}")
            .Send();
        FinishDeferredResponse();
    }

    void SendResponse(HttpStatus status, std::string body) {
        if (!deferred_) {
            return;
        }
        deferred_->Response().Status(status)
            .KeepAlive(false)
            .ContentType("text/plain")
            .Body(std::move(body))
            .Send();
    }

    void FinishDeferredResponse() {
        Cleanup();
        if (deferred_) {
            deferred_->Complete();
            deferred_.reset();
        }
    }

    void Cleanup() {
        if (cleanup_) {
            auto cleanup = std::move(cleanup_);
            cleanup();
        }
    }

    void Close() {
        if (upload_.fd >= 0) {
            close(upload_.fd);
            upload_.fd = -1;
        }
    }

    UploadState upload_;
    iouring_runtime::core::ring::IoRing* ring_ = nullptr;
    std::optional<DeferredResponse> deferred_;
    std::unordered_map<iouring_runtime::core::ring::WriteEvent*,
                       PendingWrite*> pending_;
    std::function<void()> cleanup_;
};

class UploadRegistry {
public:
    void Put(std::string request_id, std::shared_ptr<AsyncUploadState> state) {
        std::scoped_lock lock(mu_);
        uploads_[std::move(request_id)] = std::move(state);
    }

    std::shared_ptr<AsyncUploadState> Get(std::string_view request_id) {
        std::scoped_lock lock(mu_);
        auto it = uploads_.find(std::string(request_id));
        return it == uploads_.end() ? nullptr : it->second;
    }

    std::shared_ptr<AsyncUploadState> Take(std::string_view request_id) {
        std::scoped_lock lock(mu_);
        auto it = uploads_.find(std::string(request_id));
        if (it == uploads_.end()) {
            return nullptr;
        }
        auto state = std::move(it->second);
        uploads_.erase(it);
        return state;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<AsyncUploadState>> uploads_;
};

template <typename T>
T ReadUnsignedEnv(const char* name, T fallback) {
    if (const char* raw = std::getenv(name)) {
        const auto value = std::stoull(raw);
        if (value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
            return fallback;
        }
        return static_cast<T>(value);
    }
    return fallback;
}

std::string ReadStringEnv(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

std::chrono::milliseconds ReadMillisecondsEnv(
    const char* name, std::chrono::milliseconds fallback) {
    if (const char* raw = std::getenv(name)) {
        return std::chrono::milliseconds(std::stoll(raw));
    }
    return fallback;
}

using WorkerAffinityMode = iouring_runtime::web::WebServerConfig::WorkerAffinityMode;

WorkerAffinityMode ReadWorkerAffinityEnv(const char* name,
                                         WorkerAffinityMode fallback) {
    if (const char* raw = std::getenv(name)) {
        const std::string value = raw;
        if (value == "off") return WorkerAffinityMode::kOff;
        if (value == "physical") return WorkerAffinityMode::kPhysicalCores;
        if (value == "logical") return WorkerAffinityMode::kLogicalCpus;
    }
    return fallback;
}

void ConfigureLoggingFromEnv() {
    iouring_runtime::observability::ConfigureLoggingFromEnv(
        "FILE_STORE_LOG_LEVEL");
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            out += static_cast<unsigned char>(ch) < 0x20 ? ' ' : ch;
            break;
        }
    }
    return out;
}

std::string Html() {
    return R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>mintcocoa files</title>
<style>
body{margin:0;background:#111418;color:#e8eaed;font:14px system-ui,sans-serif}
main{max-width:980px;margin:0 auto;padding:28px 18px}
h1{font-size:26px;margin:0 0 18px}.bar{display:flex;gap:8px;align-items:center;margin-bottom:18px}
input[type=file]{flex:1}input[type=password]{width:220px}
button{background:#238636;border:0;color:white;border-radius:6px;padding:8px 12px;cursor:pointer}
button:disabled{background:#374151;color:#9aa4af;cursor:not-allowed}button.danger{background:#da3633}
.progress{display:none;margin:-4px 0 18px}.meter{height:10px;background:#252b33;border-radius:999px;overflow:hidden}
.fill{height:100%;width:0;background:#2f81f7;transition:width .12s linear}
.status{display:flex;justify-content:space-between;gap:12px;margin-top:8px;color:#9aa4af;font-size:13px}
table{width:100%;border-collapse:collapse;background:#171b20}
th,td{text-align:left;border-bottom:1px solid #30363d;padding:10px}a{color:#58a6ff;text-decoration:none}
.muted{color:#9aa4af}.right{text-align:right}
</style></head><body><main>
<h1>mintcocoa files</h1>
<div class="bar"><input id="file" type="file"><input id="token" type="password" placeholder="token"><button id="upload">Upload</button></div>
<div id="progress" class="progress"><div class="meter"><div id="fill" class="fill"></div></div><div class="status"><span id="state">Idle</span><span id="detail"></span></div></div>
<table><thead><tr><th>Name</th><th class="right">Size</th><th>Modified</th><th></th></tr></thead><tbody id="rows"></tbody></table>
<script>
const rows=document.getElementById('rows'), token=document.getElementById('token'), file=document.getElementById('file'), upload=document.getElementById('upload'), progress=document.getElementById('progress'), fill=document.getElementById('fill'), state=document.getElementById('state'), detail=document.getElementById('detail');
function auth(){return token.value?{'Authorization':'Bearer '+token.value}:{}};
function size(n){const u=['B','KiB','MiB','GiB'];let i=0;while(n>=1024&&i<u.length-1){n/=1024;i++}return n.toFixed(i?1:0)+' '+u[i]}
async function refresh(){const r=await fetch('/api/files');const files=await r.json();rows.innerHTML='';for(const f of files){const tr=document.createElement('tr');tr.innerHTML=`<td><a href="/files/${encodeURIComponent(f.path)}">${f.path}</a></td><td class="right">${size(f.size)}</td><td class="muted">${new Date(f.modified_ms).toLocaleString()}</td><td class="right"><button class="danger">Delete</button></td>`;tr.querySelector('button').onclick=async()=>{await fetch('/api/files/'+encodeURIComponent(f.path),{method:'DELETE',headers:auth()});refresh()};rows.appendChild(tr)}}
function setProgress(p,label,extra){progress.style.display='block';fill.style.width=Math.max(0,Math.min(100,p))+'%';state.textContent=label;detail.textContent=extra||''}
function uploadFile(f){return new Promise((resolve,reject)=>{const xhr=new XMLHttpRequest();const started=performance.now();xhr.open('PUT','/api/files/'+encodeURIComponent(f.name));xhr.setRequestHeader('Content-Type','application/octet-stream');for(const [k,v] of Object.entries(auth()))xhr.setRequestHeader(k,v);xhr.upload.onprogress=e=>{const sent=e.loaded||0,total=e.lengthComputable?e.total:f.size;const pct=total?sent*100/total:0;const seconds=Math.max((performance.now()-started)/1000,.001);setProgress(pct,'Uploading '+pct.toFixed(1)+'%',`${size(sent)} / ${size(total)} | ${size(sent/seconds)}/s`)};xhr.onload=()=>{if(xhr.status>=200&&xhr.status<300){setProgress(100,'Complete',size(f.size));resolve()}else{reject(new Error(xhr.responseText||('HTTP '+xhr.status)))}};xhr.onerror=()=>reject(new Error('Network error'));xhr.onabort=()=>reject(new Error('Upload aborted'));xhr.send(f)})}
upload.onclick=async()=>{const f=file.files[0];if(!f)return;upload.disabled=true;setProgress(0,'Starting',f.name);try{await uploadFile(f);await refresh()}catch(e){setProgress(0,'Failed',e.message)}finally{upload.disabled=false}};
refresh();
</script></main></body></html>)HTML";
}

bool Authorized(const RequestContext& ctx, std::string_view token) {
    if (token.empty()) {
        return true;
    }
    const auto header = ctx.request.GetHeader("Authorization");
    const std::string expected = "Bearer " + std::string(token);
    return header == expected;
}

bool SafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> RequestPathParam(const RequestContext& ctx) {
    std::filesystem::path path{std::string(ctx.request.Param("path"))};
    if (!SafeRelativePath(path)) {
        return std::nullopt;
    }
    return path;
}

std::filesystem::path ResolvePath(const std::filesystem::path& root,
                                  const std::filesystem::path& relative) {
    return root / relative;
}

std::string MimeType(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".txt" || ext == ".log") return "text/plain; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".pdf") return "application/pdf";
    return "application/octet-stream";
}

void ListFiles(RequestContext& ctx, const std::filesystem::path& root) {
    std::ostringstream json;
    json << "[";
    bool first = true;
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec || !entry.is_regular_file(ec)) {
                continue;
            }
            const auto rel = std::filesystem::relative(entry.path(), root, ec);
            if (ec || rel.filename().string().ends_with(".part")) {
                continue;
            }
            const auto size = entry.file_size(ec);
            if (ec) {
                continue;
            }
            const auto modified = entry.last_write_time(ec);
            const auto modified_system = std::chrono::time_point_cast<
                std::chrono::system_clock::duration>(
                modified - decltype(modified)::clock::now() +
                std::chrono::system_clock::now());
            const auto modified_ms = ec ? 0 : std::chrono::duration_cast<
                std::chrono::milliseconds>(modified_system.time_since_epoch()).count();
            json << (first ? "" : ",");
            first = false;
            json << "{\"path\":\"" << JsonEscape(rel.generic_string())
                 << "\",\"size\":" << size
                 << ",\"modified_ms\":" << modified_ms << "}";
        }
    }
    json << "]";
    ctx.response.Json(json.str()).Send();
}

void SendFile(RequestContext& ctx, const std::filesystem::path& root,
              std::uint32_t chunk_bytes) {
    const auto relative = RequestPathParam(ctx);
    if (!relative) {
        ctx.response.Status(HttpStatus::kBadRequest)
            .ContentType("text/plain")
            .Body("Bad Request")
            .Send();
        return;
    }

    const auto path = ResolvePath(root, *relative);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        ctx.response.Status(HttpStatus::kNotFound)
            .ContentType("text/plain")
            .Body("Not Found")
            .Send();
        return;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        ctx.response.Status(HttpStatus::kInternalServerError)
            .ContentType("text/plain")
            .Body("Internal Server Error")
            .Send();
        return;
    }

    ctx.response.ContentType(MimeType(path))
        .Header("Content-Length", std::to_string(size))
        .Header("Content-Disposition", "attachment; filename=\"" +
                                           JsonEscape(relative->filename().string()) + "\"")
        .Send();
    if (ctx.request.method == HttpMethod::kHead) {
        return;
    }

    std::ifstream file(path, std::ios::binary);
    std::string chunk;
    chunk.resize(std::max<std::uint32_t>(4096, chunk_bytes));
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = file.gcount();
        if (got <= 0) {
            break;
        }
        auto allocated = ctx.pool.Allocate(static_cast<std::uint32_t>(got));
        if (!allocated) {
            break;
        }
        auto buffer = std::move(*allocated);
        std::memcpy(buffer->Writable().data(), chunk.data(), static_cast<std::size_t>(got));
        buffer->Commit(static_cast<std::uint32_t>(got));
        ctx.session.SendResponse(std::move(buffer));
    }
}

void DeleteFile(RequestContext& ctx, const std::filesystem::path& root,
                std::string_view token) {
    if (!Authorized(ctx, token)) {
        ctx.response.Status(HttpStatus::kUnauthorized)
            .ContentType("text/plain")
            .Body("Unauthorized")
            .Send();
        return;
    }
    const auto relative = RequestPathParam(ctx);
    if (!relative) {
        ctx.response.Status(HttpStatus::kBadRequest)
            .ContentType("text/plain")
            .Body("Bad Request")
            .Send();
        return;
    }
    std::error_code ec;
    const auto removed = std::filesystem::remove(ResolvePath(root, *relative), ec);
    if (ec || !removed) {
        ctx.response.Status(HttpStatus::kNotFound)
            .ContentType("text/plain")
            .Body("Not Found")
            .Send();
        return;
    }
    ctx.response.Status(HttpStatus::kNoContent).Send();
}

} // namespace

int main() {
    ConfigureLoggingFromEnv();

    const auto root = std::filesystem::path(
        ReadStringEnv("FILE_STORE_ROOT", "/tmp/iouring-runtime-files"));
    const auto auth_token = ReadStringEnv("FILE_STORE_AUTH_TOKEN", "");
    const auto max_upload_bytes =
        ReadUnsignedEnv<std::uint64_t>("FILE_STORE_MAX_UPLOAD_BYTES",
                                       kDefaultMaxUploadBytes);
    const auto send_chunk_bytes =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_SEND_CHUNK_BYTES",
                                       kDefaultSendChunkBytes);
    std::filesystem::create_directories(root);

    iouring_runtime::web::WebServerConfig config;
    config.host = ReadStringEnv("FILE_STORE_HOST", "127.0.0.1");
    config.port = ReadUnsignedEnv<std::uint16_t>("FILE_STORE_PORT", 3012);
    config.worker_count = ReadUnsignedEnv<std::uint16_t>("FILE_STORE_WORKERS", 1);
    config.worker_affinity =
        ReadWorkerAffinityEnv("FILE_STORE_WORKER_AFFINITY", config.worker_affinity);
    config.max_sessions_per_worker =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_MAX_SESSIONS_PER_WORKER", 0);
    config.ring.queue_depth =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_RING_QUEUE_DEPTH",
                                       config.ring.queue_depth);
    config.ring.buf_count =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_RING_BUF_COUNT",
                                       config.ring.buf_count);
    config.ring.buf_size =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_RING_BUF_SIZE",
                                       config.ring.buf_size);
    config.ring.submit_batch_size =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_RING_SUBMIT_BATCH",
                                       config.ring.submit_batch_size);
    config.ring.cqe_batch_budget =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_RING_CQE_BATCH_BUDGET",
                                       config.ring.cqe_batch_budget);
    config.ring.io_timeout =
        ReadMillisecondsEnv("FILE_STORE_RING_IO_TIMEOUT_MS", config.ring.io_timeout);
    config.parser.max_body_bytes = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(max_upload_bytes,
                                std::numeric_limits<std::uint32_t>::max()));
    config.timeouts.inactivity =
        ReadMillisecondsEnv("FILE_STORE_INACTIVITY_TIMEOUT_MS",
                            std::chrono::milliseconds{60000});
    config.timeouts.request =
        ReadMillisecondsEnv("FILE_STORE_REQUEST_TIMEOUT_MS",
                            std::chrono::milliseconds{120000});
    config.backpressure.send_queue_max_pending =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_SEND_QUEUE_MAX_PENDING",
                                       config.backpressure.send_queue_max_pending);
    config.backpressure.send_queue_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_SEND_QUEUE_HIGH_WATERMARK",
                                       config.backpressure.send_queue_high_watermark);
    config.backpressure.send_queue_low_watermark =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_SEND_QUEUE_LOW_WATERMARK",
                                       config.backpressure.send_queue_low_watermark);
    config.backpressure.disconnect_on_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("FILE_STORE_DISCONNECT_ON_HIGH_WATERMARK", 0) != 0;

    UploadRegistry uploads;
    iouring_runtime::web::WebServer server(config);
    iouring_runtime::web::WebServer::InstallStopSignalHandlers();

    server.Get("/", [](RequestContext& ctx) {
        ctx.response.ContentType("text/html; charset=utf-8")
            .Body(Html())
            .Send();
    });
    server.Get("/api/files", [&](RequestContext& ctx) {
        ListFiles(ctx, root);
    });
    server.Get("/files/*path", [&](RequestContext& ctx) {
        SendFile(ctx, root, send_chunk_bytes);
    });
    server.Head("/files/*path", [&](RequestContext& ctx) {
        SendFile(ctx, root, send_chunk_bytes);
    });
    server.Delete("/api/files/*path", [&](RequestContext& ctx) {
        DeleteFile(ctx, root, auth_token);
    });

    server.PutStream("/api/files/*path", HttpStreamHandler{
        .on_headers = [&](RequestContext& ctx) {
            if (!Authorized(ctx, auth_token)) {
                ctx.response.Status(HttpStatus::kUnauthorized)
                    .ContentType("text/plain")
                    .Body("Unauthorized")
                    .Send();
                return false;
            }
            const auto relative = RequestPathParam(ctx);
            if (!relative) {
                ctx.response.Status(HttpStatus::kBadRequest)
                    .ContentType("text/plain")
                    .Body("Bad Request")
                    .Send();
                return false;
            }
            const auto content_length = ctx.request.ContentLength();
            if (content_length > max_upload_bytes) {
                ctx.response.Status(HttpStatus::kPayloadTooLarge)
                    .ContentType("text/plain")
                    .Body("Payload Too Large")
                    .Send();
                return false;
            }

            UploadState upload;
            upload.expected_bytes = content_length;
            upload.final_path = ResolvePath(root, *relative);
            upload.temp_path = upload.final_path;
            upload.temp_path += ".part";
            std::filesystem::create_directories(upload.final_path.parent_path());
            upload.fd = open(upload.temp_path.c_str(),
                             O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
            if (upload.fd < 0) {
                ctx.response.Status(HttpStatus::kInternalServerError)
                    .ContentType("text/plain")
                    .Body("open failed")
                    .Send();
                return false;
            }
            const std::string request_id(ctx.request_id);
            auto state = std::make_shared<AsyncUploadState>();
            state->Start(ctx, std::move(upload), [&uploads, request_id] {
                uploads.Take(request_id);
            });
            uploads.Put(request_id, std::move(state));
            return true;
        },
        .on_body = [&](RequestContext& ctx, std::span<const std::byte> chunk) {
            auto state = uploads.Get(ctx.request_id);
            return state ? state->SubmitChunk(chunk) : true;
        },
        .on_complete = [&](RequestContext& ctx) {
            auto deferred = ctx.DeferResponse();
            auto state = uploads.Get(ctx.request_id);
            if (state) {
                state->MarkRequestComplete(std::move(deferred));
            }
        },
        .on_abort = [&](RequestContext& ctx) {
            auto state = uploads.Get(ctx.request_id);
            if (!state) {
                return;
            }
            state->Abort();
        },
    });

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
    return 0;
}
