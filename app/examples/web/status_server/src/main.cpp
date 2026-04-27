#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/web/WebServer.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

using iouring_runtime::web::RequestContext;

namespace {

struct MemoryInfo {
    std::uint64_t total_kb = 0;
    std::uint64_t available_kb = 0;
    std::uint64_t swap_total_kb = 0;
    std::uint64_t swap_free_kb = 0;
};

struct ProcessInfo {
    int pid = 0;
    std::string name;
    std::string state;
    std::uint64_t vm_rss_kb = 0;
    std::uint64_t vm_size_kb = 0;
};

struct ProxyRuntimeWorker {
    std::uint64_t index = 0;
    int pinned_cpu = -1;
    std::uint64_t live_sessions = 0;
    std::uint64_t live_connectors = 0;
};

struct ProxyRuntimeRoute {
    std::string hostname;
    std::string upstream;
};

struct ProxyRuntimeSnapshot {
    bool valid = false;
    std::string raw_json;
    std::string error;
    std::string default_upstream;
    std::string listen_host;
    std::uint64_t listen_port = 0;
    std::uint64_t pid = 0;
    std::uint64_t uptime_seconds = 0;
    std::uint64_t configured_worker_count = 0;
    std::uint64_t running_worker_count = 0;
    std::uint64_t total_live_sessions = 0;
    std::uint64_t total_live_connectors = 0;
    bool tls_enabled = false;
    bool tls_context_loaded = false;
    std::vector<ProxyRuntimeWorker> workers;
    std::vector<ProxyRuntimeRoute> routes;
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

WorkerAffinityMode ReadWorkerAffinityEnv(
    const char* name, WorkerAffinityMode fallback) {
    if (const char* raw = std::getenv(name)) {
        const std::string value = raw;
        if (value == "off") {
            return WorkerAffinityMode::kOff;
        }
        if (value == "physical") {
            return WorkerAffinityMode::kPhysicalCores;
        }
        if (value == "logical") {
            return WorkerAffinityMode::kLogicalCpus;
        }
    }
    return fallback;
}

std::string ReadFile(std::string_view path) {
    std::ifstream file{std::string(path)};
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out += ' ';
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

std::string HtmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out += ' ';
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string JsonKey(std::string_view key) {
    return "\"" + std::string(key) + "\"";
}

std::optional<std::uint64_t> ExtractJsonUint(std::string_view json,
                                             std::string_view key) {
    const auto key_pos = json.find(JsonKey(key));
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = json.find(':', key_pos);
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    std::uint64_t value = 0;
    bool any = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + static_cast<unsigned>(json[pos] - '0');
        any = true;
        ++pos;
    }
    if (!any) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> ExtractJsonInt(std::string_view json, std::string_view key) {
    const auto key_pos = json.find(JsonKey(key));
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = json.find(':', key_pos);
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    int sign = 1;
    if (pos < json.size() && json[pos] == '-') {
        sign = -1;
        ++pos;
    }
    int value = 0;
    bool any = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + static_cast<unsigned>(json[pos] - '0');
        any = true;
        ++pos;
    }
    if (!any) {
        return std::nullopt;
    }
    return value * sign;
}

std::optional<bool> ExtractJsonBool(std::string_view json, std::string_view key) {
    const auto key_pos = json.find(JsonKey(key));
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = json.find(':', key_pos);
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto value = Trim(json.substr(colon + 1));
    if (value.starts_with("true")) {
        return true;
    }
    if (value.starts_with("false")) {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> ExtractJsonString(std::string_view json,
                                             std::string_view key) {
    const auto key_pos = json.find(JsonKey(key));
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = json.find(':', key_pos);
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string out;
    while (pos < json.size()) {
        const char ch = json[pos++];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\' && pos < json.size()) {
            out.push_back(json[pos++]);
        } else {
            out.push_back(ch);
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> ExtractJsonArray(std::string_view json,
                                                 std::string_view key) {
    const auto key_pos = json.find(JsonKey(key));
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = json.find(':', key_pos);
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto start = json.find('[', colon);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (auto pos = start; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0) {
                return json.substr(start + 1, pos - start - 1);
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> SplitJsonObjects(std::string_view array) {
    std::vector<std::string_view> objects;
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    std::size_t object_start = std::string_view::npos;
    for (std::size_t pos = 0; pos < array.size(); ++pos) {
        const char ch = array[pos];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            if (depth == 0) {
                object_start = pos;
            }
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0 && object_start != std::string_view::npos) {
                objects.push_back(array.substr(object_start, pos - object_start + 1));
                object_start = std::string_view::npos;
            }
        }
    }
    return objects;
}

class JsonSyntaxValidator {
public:
    explicit JsonSyntaxValidator(std::string_view json) : json_(json) {}

    bool Valid() {
        SkipWhitespace();
        if (!ConsumeValue()) {
            return false;
        }
        SkipWhitespace();
        return pos_ == json_.size();
    }

private:
    void SkipWhitespace() {
        while (pos_ < json_.size() &&
               std::isspace(static_cast<unsigned char>(json_[pos_]))) {
            ++pos_;
        }
    }

    bool ConsumeValue() {
        SkipWhitespace();
        if (pos_ >= json_.size()) {
            return false;
        }
        switch (json_[pos_]) {
        case '{':
            return ConsumeObject();
        case '[':
            return ConsumeArray();
        case '"':
            return ConsumeString();
        case 't':
            return ConsumeLiteral("true");
        case 'f':
            return ConsumeLiteral("false");
        case 'n':
            return ConsumeLiteral("null");
        default:
            return ConsumeNumber();
        }
    }

    bool ConsumeObject() {
        ++pos_;
        SkipWhitespace();
        if (pos_ < json_.size() && json_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipWhitespace();
            if (!ConsumeString()) {
                return false;
            }
            SkipWhitespace();
            if (pos_ >= json_.size() || json_[pos_] != ':') {
                return false;
            }
            ++pos_;
            if (!ConsumeValue()) {
                return false;
            }
            SkipWhitespace();
            if (pos_ < json_.size() && json_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < json_.size() && json_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool ConsumeArray() {
        ++pos_;
        SkipWhitespace();
        if (pos_ < json_.size() && json_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            if (!ConsumeValue()) {
                return false;
            }
            SkipWhitespace();
            if (pos_ < json_.size() && json_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < json_.size() && json_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool ConsumeString() {
        if (pos_ >= json_.size() || json_[pos_] != '"') {
            return false;
        }
        ++pos_;
        while (pos_ < json_.size()) {
            const char ch = json_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                return false;
            }
            if (ch != '\\') {
                continue;
            }
            if (pos_ >= json_.size()) {
                return false;
            }
            const char escaped = json_[pos_++];
            if (escaped == 'u') {
                for (int i = 0; i < 4; ++i) {
                    if (pos_ >= json_.size() ||
                        !std::isxdigit(static_cast<unsigned char>(json_[pos_]))) {
                        return false;
                    }
                    ++pos_;
                }
                continue;
            }
            if (std::string_view("\"\\/bfnrt").find(escaped) ==
                std::string_view::npos) {
                return false;
            }
        }
        return false;
    }

    bool ConsumeNumber() {
        const auto start = pos_;
        if (pos_ < json_.size() && json_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= json_.size()) {
            return false;
        }
        if (json_[pos_] == '0') {
            ++pos_;
        } else if (std::isdigit(static_cast<unsigned char>(json_[pos_]))) {
            while (pos_ < json_.size() &&
                   std::isdigit(static_cast<unsigned char>(json_[pos_]))) {
                ++pos_;
            }
        } else {
            return false;
        }
        if (pos_ < json_.size() && json_[pos_] == '.') {
            ++pos_;
            const auto fraction_start = pos_;
            while (pos_ < json_.size() &&
                   std::isdigit(static_cast<unsigned char>(json_[pos_]))) {
                ++pos_;
            }
            if (fraction_start == pos_) {
                return false;
            }
        }
        if (pos_ < json_.size() && (json_[pos_] == 'e' || json_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < json_.size() && (json_[pos_] == '+' || json_[pos_] == '-')) {
                ++pos_;
            }
            const auto exponent_start = pos_;
            while (pos_ < json_.size() &&
                   std::isdigit(static_cast<unsigned char>(json_[pos_]))) {
                ++pos_;
            }
            if (exponent_start == pos_) {
                return false;
            }
        }
        return start != pos_;
    }

    bool ConsumeLiteral(std::string_view literal) {
        if (json_.substr(pos_, literal.size()) != literal) {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    std::string_view json_;
    std::size_t pos_ = 0;
};

std::uint64_t ParseKbValue(std::string_view line) {
    std::uint64_t value = 0;
    for (const char ch : line) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            value = value * 10 + static_cast<unsigned>(ch - '0');
        } else if (value != 0) {
            break;
        }
    }
    return value;
}

MemoryInfo ReadMemoryInfo() {
    MemoryInfo info;
    std::ifstream file("/proc/meminfo");
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (file >> key >> value >> unit) {
        if (key == "MemTotal:") {
            info.total_kb = value;
        } else if (key == "MemAvailable:") {
            info.available_kb = value;
        } else if (key == "SwapTotal:") {
            info.swap_total_kb = value;
        } else if (key == "SwapFree:") {
            info.swap_free_kb = value;
        }
    }
    return info;
}

std::string ReadLoadAverage() {
    std::ifstream file("/proc/loadavg");
    std::string one;
    std::string five;
    std::string fifteen;
    file >> one >> five >> fifteen;
    if (one.empty()) {
        return "unavailable";
    }
    return one + " " + five + " " + fifteen;
}

std::string ReadHostname() {
    auto host = ReadFile("/proc/sys/kernel/hostname");
    while (!host.empty() && std::isspace(static_cast<unsigned char>(host.back()))) {
        host.pop_back();
    }
    return host.empty() ? "unknown" : host;
}

std::optional<ProcessInfo> ReadProcessInfo(int pid) {
    const std::string base = "/proc/" + std::to_string(pid);
    std::ifstream status(base + "/status");
    if (!status) {
        return std::nullopt;
    }

    ProcessInfo info;
    info.pid = pid;
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("Name:")) {
            info.name = line.substr(5);
            while (!info.name.empty() &&
                   std::isspace(static_cast<unsigned char>(info.name.front()))) {
                info.name.erase(info.name.begin());
            }
        } else if (line.starts_with("State:")) {
            info.state = line.substr(6);
            while (!info.state.empty() &&
                   std::isspace(static_cast<unsigned char>(info.state.front()))) {
                info.state.erase(info.state.begin());
            }
        } else if (line.starts_with("VmRSS:")) {
            info.vm_rss_kb = ParseKbValue(line);
        } else if (line.starts_with("VmSize:")) {
            info.vm_size_kb = ParseKbValue(line);
        }
    }
    return info;
}

std::optional<ProcessInfo> FindProcess(std::string_view needle) {
    namespace fs = std::filesystem;
    std::optional<ProcessInfo> fallback;
    for (const auto& entry : fs::directory_iterator("/proc")) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (filename.empty() ||
            !std::all_of(filename.begin(), filename.end(), [](char ch) {
                return std::isdigit(static_cast<unsigned char>(ch));
            })) {
            continue;
        }

        const int pid = std::stoi(filename);
        std::error_code ec;
        const auto exe = fs::read_symlink(entry.path() / "exe", ec);
        if (!ec && exe.filename() == needle) {
            return ReadProcessInfo(pid);
        }

        const auto cmdline = ReadFile(entry.path().string() + "/cmdline");
        const auto first_arg_end = cmdline.find('\0');
        const auto first_arg =
            cmdline.substr(0, first_arg_end == std::string::npos ? cmdline.size()
                                                                 : first_arg_end);
        if (!first_arg.empty() && fs::path(first_arg).filename() == needle) {
            return ReadProcessInfo(pid);
        }
        if (!fallback && cmdline.find(needle) != std::string::npos) {
            fallback = ReadProcessInfo(pid);
        }
    }
    return fallback;
}

ProxyRuntimeSnapshot ReadProxyRuntimeSnapshot() {
    const auto path = ReadStringEnv(
        "STATUS_PROXY_METRICS_FILE",
        "/run/iouring-runtime/tcp_reverse_proxy.metrics.json");
    ProxyRuntimeSnapshot snapshot;
    snapshot.raw_json = ReadFile(path);
    const auto trimmed = Trim(snapshot.raw_json);
    if (trimmed.empty()) {
        snapshot.error = "metrics file not readable: " + path;
        return snapshot;
    }
    if (!trimmed.starts_with("{") || !trimmed.ends_with("}")) {
        snapshot.error = "metrics file is not a JSON object: " + path;
        return snapshot;
    }
    if (!JsonSyntaxValidator(trimmed).Valid()) {
        snapshot.error = "metrics file contains invalid JSON: " + path;
        return snapshot;
    }
    const auto service = ExtractJsonString(trimmed, "service");
    if (!service || *service != "tcp_reverse_proxy") {
        snapshot.error = "metrics file is not a tcp_reverse_proxy snapshot: " + path;
        return snapshot;
    }

    snapshot.valid = true;
    if (auto value = ExtractJsonUint(trimmed, "pid")) {
        snapshot.pid = *value;
    }
    if (auto value = ExtractJsonUint(trimmed, "uptime_seconds")) {
        snapshot.uptime_seconds = *value;
    }
    if (auto value = ExtractJsonUint(trimmed, "configured_worker_count")) {
        snapshot.configured_worker_count = *value;
    }
    if (auto value = ExtractJsonUint(trimmed, "running_worker_count")) {
        snapshot.running_worker_count = *value;
    }
    if (auto value = ExtractJsonUint(trimmed, "total_live_sessions")) {
        snapshot.total_live_sessions = *value;
    }
    if (auto value = ExtractJsonUint(trimmed, "total_live_connectors")) {
        snapshot.total_live_connectors = *value;
    }
    if (auto value = ExtractJsonString(trimmed, "default_upstream")) {
        snapshot.default_upstream = std::move(*value);
    }
    if (auto listen = ExtractJsonString(trimmed, "host")) {
        snapshot.listen_host = std::move(*listen);
    }
    if (auto value = ExtractJsonUint(trimmed, "port")) {
        snapshot.listen_port = *value;
    }
    if (auto tls = trimmed.find("\"tls\""); tls != std::string_view::npos) {
        const auto tls_json = trimmed.substr(tls);
        if (auto value = ExtractJsonBool(tls_json, "enabled")) {
            snapshot.tls_enabled = *value;
        }
        if (auto value = ExtractJsonBool(tls_json, "context_loaded")) {
            snapshot.tls_context_loaded = *value;
        }
    }

    if (auto workers = ExtractJsonArray(trimmed, "workers")) {
        for (const auto worker_json : SplitJsonObjects(*workers)) {
            ProxyRuntimeWorker worker;
            if (auto value = ExtractJsonUint(worker_json, "index")) {
                worker.index = *value;
            }
            if (auto value = ExtractJsonInt(worker_json, "pinned_cpu")) {
                worker.pinned_cpu = *value;
            }
            if (auto value = ExtractJsonUint(worker_json, "live_sessions")) {
                worker.live_sessions = *value;
            }
            if (auto value = ExtractJsonUint(worker_json, "live_connectors")) {
                worker.live_connectors = *value;
            }
            snapshot.workers.push_back(worker);
        }
    }

    if (auto routes = ExtractJsonArray(trimmed, "configured_routes")) {
        for (const auto route_json : SplitJsonObjects(*routes)) {
            ProxyRuntimeRoute route;
            if (auto value = ExtractJsonString(route_json, "hostname")) {
                route.hostname = std::move(*value);
            }
            if (auto value = ExtractJsonString(route_json, "upstream")) {
                route.upstream = std::move(*value);
            }
            snapshot.routes.push_back(route);
        }
    }
    return snapshot;
}

std::string FormatMiB(std::uint64_t kb) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << static_cast<double>(kb) / 1024.0;
    return out.str();
}

std::string BuildMetricsJson() {
    const auto mem = ReadMemoryInfo();
    const auto proxy = FindProcess("tcp_reverse_proxy");
    const auto proxy_runtime = ReadProxyRuntimeSnapshot();
    std::ostringstream json;
    json << "{";
    json << "\"service\":\"status_server\",";
    json << "\"hostname\":\"" << JsonEscape(ReadHostname()) << "\",";
    json << "\"loadavg\":\"" << JsonEscape(ReadLoadAverage()) << "\",";
    json << "\"memory\":{";
    json << "\"total_kb\":" << mem.total_kb << ",";
    json << "\"available_kb\":" << mem.available_kb << ",";
    json << "\"swap_total_kb\":" << mem.swap_total_kb << ",";
    json << "\"swap_free_kb\":" << mem.swap_free_kb;
    json << "},";
    json << "\"proxy\":";
    if (proxy) {
        json << "{";
        json << "\"pid\":" << proxy->pid << ",";
        json << "\"name\":\"" << JsonEscape(proxy->name) << "\",";
        json << "\"state\":\"" << JsonEscape(proxy->state) << "\",";
        json << "\"rss_kb\":" << proxy->vm_rss_kb << ",";
        json << "\"vmsize_kb\":" << proxy->vm_size_kb;
        json << "}";
    } else {
        json << "null";
    }
    json << ",";
    json << "\"proxy_runtime\":";
    if (proxy_runtime.valid) {
        json << proxy_runtime.raw_json;
    } else {
        json << "null";
    }
    json << ",";
    json << "\"proxy_runtime_error\":";
    if (proxy_runtime.valid) {
        json << "null";
    } else {
        json << "\"" << JsonEscape(proxy_runtime.error) << "\"";
    }
    json << "}";
    return json.str();
}

std::string BuildDashboardHtml() {
    const auto mem = ReadMemoryInfo();
    const auto proxy = FindProcess("tcp_reverse_proxy");
    const auto proxy_runtime = ReadProxyRuntimeSnapshot();
    const auto used_kb = mem.total_kb > mem.available_kb
                             ? mem.total_kb - mem.available_kb
                             : 0;
    const auto used_pct =
        mem.total_kb == 0 ? 0.0 : (static_cast<double>(used_kb) * 100.0 / mem.total_kb);

    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\">";
    html << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html << "<title>mintcocoa status</title>";
    html << "<style>";
    html << "body{margin:0;background:#101214;color:#e8eaed;font:14px system-ui,sans-serif}";
    html << "main{max-width:980px;margin:0 auto;padding:32px 20px}";
    html << "h1{font-size:28px;margin:0 0 24px}h2{font-size:13px;color:#a7b0ba;margin:0 0 8px;text-transform:uppercase}";
    html << ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}";
    html << ".card{background:#191d21;border:1px solid #30363d;border-radius:8px;padding:16px}";
    html << ".value{font-size:28px;font-weight:700;margin:6px 0}.muted{color:#a7b0ba}";
    html << ".ok{color:#7ee787}.warn{color:#f2cc60}.bar{height:8px;background:#30363d;border-radius:999px;overflow:hidden;margin-top:12px}";
    html << ".fill{height:100%;background:#58a6ff;width:" << used_pct << "%}";
    html << "a{color:#58a6ff;text-decoration:none}code{color:#c9d1d9}";
    html << "table{width:100%;border-collapse:collapse;margin-top:8px}td,th{text-align:left;border-bottom:1px solid #30363d;padding:6px 4px}";
    html << "th{color:#a7b0ba;font-size:12px;font-weight:600}";
    html << "</style></head><body><main>";
    html << "<h1>mintcocoa runtime status</h1>";
    html << "<div class=\"grid\">";
    html << "<section class=\"card\"><h2>Host</h2><div class=\"value\">"
         << HtmlEscape(ReadHostname()) << "</div><div class=\"muted\">load "
         << HtmlEscape(ReadLoadAverage()) << "</div></section>";
    html << "<section class=\"card\"><h2>Memory</h2><div class=\"value\">"
         << FormatMiB(used_kb) << " MiB</div><div class=\"muted\">available "
         << FormatMiB(mem.available_kb) << " MiB / total " << FormatMiB(mem.total_kb)
         << " MiB</div><div class=\"bar\"><div class=\"fill\"></div></div></section>";
    html << "<section class=\"card\"><h2>Proxy</h2>";
    if (proxy) {
        html << "<div class=\"value ok\">running</div><div class=\"muted\">pid "
             << proxy->pid << ", rss " << FormatMiB(proxy->vm_rss_kb)
             << " MiB, state " << HtmlEscape(proxy->state) << "</div>";
    } else {
        html << "<div class=\"value warn\">not found</div><div class=\"muted\">tcp_reverse_proxy process not visible</div>";
    }
    html << "</section>";
    html << "<section class=\"card\"><h2>Proxy runtime</h2>";
    if (proxy_runtime.valid) {
        html << "<div class=\"value ok\">"
             << proxy_runtime.total_live_sessions << " sessions</div>";
        html << "<div class=\"muted\">listen "
             << HtmlEscape(proxy_runtime.listen_host) << ":"
             << proxy_runtime.listen_port << ", upstream "
             << HtmlEscape(proxy_runtime.default_upstream)
             << ", workers " << proxy_runtime.running_worker_count << "/"
             << proxy_runtime.configured_worker_count << ", tls "
             << (proxy_runtime.tls_context_loaded ? "loaded" : "off")
             << "</div>";
    } else {
        html << "<div class=\"value warn\">unavailable</div><div class=\"muted\">"
             << HtmlEscape(proxy_runtime.error) << "</div>";
    }
    html << "</section>";
    html << "<section class=\"card\"><h2>Endpoints</h2><div><a href=\"/health\">/health</a></div>";
    html << "<div><a href=\"/metrics\">/metrics</a></div></section>";
    html << "</div>";
    if (proxy_runtime.valid) {
        html << "<section class=\"card\" style=\"margin-top:12px\"><h2>Workers</h2><table><thead><tr>";
        html << "<th>Worker</th><th>CPU</th><th>Sessions</th><th>Connectors</th></tr></thead><tbody>";
        for (const auto& worker : proxy_runtime.workers) {
            html << "<tr><td>" << worker.index << "</td><td>" << worker.pinned_cpu
                 << "</td><td>" << worker.live_sessions << "</td><td>"
                 << worker.live_connectors << "</td></tr>";
        }
        html << "</tbody></table></section>";

        html << "<section class=\"card\" style=\"margin-top:12px\"><h2>Routes</h2><table><thead><tr>";
        html << "<th>Host</th><th>Upstream</th></tr></thead><tbody>";
        if (proxy_runtime.routes.empty()) {
            html << "<tr><td>default</td><td>"
                 << HtmlEscape(proxy_runtime.default_upstream) << "</td></tr>";
        } else {
            for (const auto& route : proxy_runtime.routes) {
                html << "<tr><td>" << HtmlEscape(route.hostname) << "</td><td>"
                     << HtmlEscape(route.upstream) << "</td></tr>";
            }
        }
        html << "</tbody></table></section>";
    }
    html << "</main></body></html>";
    return html.str();
}

void ConfigureLoggingFromEnv() {
    iouring_runtime::observability::ConfigureLoggingFromEnv(
        "STATUS_LOG_LEVEL");
}

} // namespace

int main() {
    ConfigureLoggingFromEnv();

    iouring_runtime::web::WebServerConfig config;
    config.port = ReadUnsignedEnv<std::uint16_t>("STATUS_PORT", 3010);
    config.host = ReadStringEnv("STATUS_HOST", "127.0.0.1");
    config.worker_count = ReadUnsignedEnv<std::uint16_t>("STATUS_WORKERS", 1);
    config.worker_affinity =
        ReadWorkerAffinityEnv("STATUS_WORKER_AFFINITY", config.worker_affinity);
    config.max_sessions_per_worker =
        ReadUnsignedEnv<std::uint32_t>("STATUS_MAX_SESSIONS_PER_WORKER", 0);
    config.ring.queue_depth =
        ReadUnsignedEnv<std::uint32_t>("STATUS_RING_QUEUE_DEPTH",
                                       config.ring.queue_depth);
    config.ring.buf_count =
        ReadUnsignedEnv<std::uint32_t>("STATUS_RING_BUF_COUNT",
                                       config.ring.buf_count);
    config.ring.buf_size =
        ReadUnsignedEnv<std::uint32_t>("STATUS_RING_BUF_SIZE",
                                       config.ring.buf_size);
    config.ring.io_timeout =
        ReadMillisecondsEnv("STATUS_RING_IO_TIMEOUT_MS", config.ring.io_timeout);
    config.timeouts.inactivity =
        ReadMillisecondsEnv("STATUS_INACTIVITY_TIMEOUT_MS", config.timeouts.inactivity);
    config.timeouts.request =
        ReadMillisecondsEnv("STATUS_REQUEST_TIMEOUT_MS", std::chrono::milliseconds{2000});

    iouring_runtime::web::WebServer server(config);
    iouring_runtime::web::WebServer::InstallStopSignalHandlers();

    server.Get("/", [](RequestContext& ctx) {
        ctx.response.ContentType("text/html; charset=utf-8")
            .Body(BuildDashboardHtml())
            .Send();
    });

    server.Get("/health", [](RequestContext& ctx) {
        ctx.response.ContentType("text/plain").Body("ok").Send();
    });

    server.Get("/metrics", [](RequestContext& ctx) {
        ctx.response.Json(BuildMetricsJson()).Send();
    });

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
    return 0;
}
