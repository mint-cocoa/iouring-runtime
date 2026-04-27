#include "ActivityUtils.h"

#include <iouring_runtime/media/Hls.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <sys/wait.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace activity_server {

std::string EnvString(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

std::uint16_t EnvPort(const char* name, std::uint16_t fallback) {
    if (const char* raw = std::getenv(name)) {
        try {
            const auto value = std::stoul(raw);
            if (value <= 65535) {
                return static_cast<std::uint16_t>(value);
            }
        } catch (...) {
        }
    }
    return fallback;
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

std::string Lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        out += static_cast<char>(std::tolower(ch));
    }
    return out;
}

std::string Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string QueryParam(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        const auto eq = part.find('=');
        const auto key = eq == std::string_view::npos ? part : part.substr(0, eq);
        if (key == name) {
            return iouring_runtime::media::UrlDecode(
                eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return {};
}

std::optional<std::string> JsonString(std::string_view json, std::string_view key) {
    const auto needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;

    std::string out;
    while (pos < json.size()) {
        const char ch = json[pos++];
        if (ch == '"') return out;
        if (ch == '\\' && pos < json.size()) {
            const char escaped = json[pos++];
            switch (escaped) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += escaped; break;
            }
            continue;
        }
        out += ch;
    }
    return std::nullopt;
}

std::optional<double> JsonNumber(std::string_view json, std::string_view key) {
    const auto needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    const auto start = pos;
    while (pos < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[pos])) ||
            json[pos] == '.' || json[pos] == '-')) {
        ++pos;
    }
    if (start == pos) return std::nullopt;
    try {
        return std::stod(std::string(json.substr(start, pos - start)));
    } catch (...) {
        return std::nullopt;
    }
}

std::string RandomId(std::string_view prefix) {
    static std::atomic<std::uint64_t> seq{1};
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << prefix << std::hex
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count()
        << seq.fetch_add(1, std::memory_order_relaxed)
        << (rng() & 0xffff);
    return out.str();
}

namespace {

std::string Base64(const unsigned char* data, std::size_t size) {
    std::string out;
    out.resize(4 * ((size + 2) / 3));
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    if (written < 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

} // namespace

std::string WebSocketAccept(std::string_view key) {
    static constexpr std::string_view kMagic =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string material = std::string(key) + std::string(kMagic);
    unsigned char digest[SHA_DIGEST_LENGTH] = {};
    SHA1(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest);
    return Base64(digest, sizeof(digest));
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::string ShellQuote(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

int RunCommand(const std::string& command) {
    return std::system(command.c_str());
}

CommandResult RunCommandCapture(const std::string& command) {
    CommandResult result;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return result;
    }

    char buffer[8192];
    while (true) {
        const auto n = std::fread(buffer, 1, sizeof(buffer), pipe);
        if (n > 0) {
            result.output.append(buffer, n);
        }
        if (n < sizeof(buffer)) {
            if (std::feof(pipe)) break;
            if (std::ferror(pipe)) break;
        }
    }

    const int status = ::pclose(pipe);
    if (status >= 0 && WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    return result;
}

bool IsHttpUrl(std::string_view url) {
    return url.starts_with("http://") || url.starts_with("https://");
}

CommandResult CurlGet(std::string_view url, std::string_view extra_args) {
    return RunCommandCapture("curl -LfsS --max-time 30 -A " +
                             ShellQuote("Mozilla/5.0 Cocoatube/1.0") + " " +
                             std::string(extra_args) + " " + ShellQuote(url) +
                             " 2>/dev/null");
}

} // namespace activity_server
