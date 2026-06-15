#include <iouring/media/Hls.h>

#include <cctype>
#include <sstream>
#include <string>

namespace iouring::media {

namespace {

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string Lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        out += static_cast<char>(std::tolower(ch));
    }
    return out;
}

bool IsHttpUrl(std::string_view url) {
    return url.starts_with("http://") || url.starts_with("https://");
}

std::string OriginOf(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};
    const auto host_start = scheme + 3;
    const auto path_start = url.find('/', host_start);
    return std::string(url.substr(0, path_start == std::string_view::npos
        ? std::string_view::npos
        : path_start));
}

bool ShouldProxyManifestUri(std::string_view uri) {
    return !uri.empty() && !uri.starts_with("data:");
}

} // namespace

std::string UrlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = HexValue(text[i + 1]);
            const int lo = HexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string UrlEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += kHex[ch >> 4];
            out += kHex[ch & 0x0f];
        }
    }
    return out;
}

std::string ResolveUrl(std::string_view base, std::string_view value) {
    if (IsHttpUrl(value)) {
        return std::string(value);
    }
    const auto origin = OriginOf(base);
    if (value.starts_with("//")) {
        const auto scheme = base.substr(0, base.find(':'));
        return std::string(scheme) + ":" + std::string(value);
    }
    if (value.starts_with('/')) {
        return origin + std::string(value);
    }

    const auto slash = base.rfind('/');
    if (slash == std::string_view::npos ||
        base.substr(0, slash).find("://") == std::string_view::npos) {
        return origin + "/" + std::string(value);
    }
    return std::string(base.substr(0, slash + 1)) + std::string(value);
}

std::string ProxyHlsUrl(std::string_view remote_url) {
    return "/proxy/hls?url=" + UrlEncode(remote_url);
}

std::string RewriteHlsManifest(std::string_view text, std::string_view base_url) {
    std::istringstream in{std::string(text)};
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            out << "\n";
            continue;
        }

        if (line.starts_with("#")) {
            std::string rewritten = line;
            std::size_t pos = 0;
            while ((pos = rewritten.find("URI=\"", pos)) != std::string::npos) {
                const auto start = pos + 5;
                const auto end = rewritten.find('"', start);
                if (end == std::string::npos) break;
                const auto uri = rewritten.substr(start, end - start);
                if (ShouldProxyManifestUri(uri)) {
                    const auto resolved = ResolveUrl(base_url, uri);
                    const auto proxied = ProxyHlsUrl(resolved);
                    rewritten.replace(start, end - start, proxied);
                    pos = start + proxied.size();
                } else {
                    pos = end + 1;
                }
            }
            out << rewritten << "\n";
            continue;
        }

        out << ProxyHlsUrl(ResolveUrl(base_url, line)) << "\n";
    }
    return out.str();
}

std::string ContentTypeForUrl(std::string_view url) {
    const auto lower = Lower(url);
    if (lower.ends_with(".m3u8")) return "application/vnd.apple.mpegurl";
    if (lower.ends_with(".mpd")) return "application/dash+xml";
    if (lower.ends_with(".ts")) return "video/mp2t";
    if (lower.ends_with(".m4s")) return "video/iso.segment";
    if (lower.ends_with(".mp4")) return "video/mp4";
    if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) return "image/jpeg";
    if (lower.ends_with(".png")) return "image/png";
    if (lower.ends_with(".webp")) return "image/webp";
    if (lower.ends_with(".gif")) return "image/gif";
    return "application/octet-stream";
}

} // namespace iouring::media
