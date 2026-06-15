#include <iouring/http/HttpRequest.h>

#include <charconv>

namespace iouring::http {

std::string_view HttpRequest::GetHeader(std::string_view name) const {
    for (const auto& header : headers_) {
        if (CaseInsensitiveEqual(header.name, name)) {
            return header.value;
        }
    }
    return {};
}

bool HttpRequest::HasHeader(std::string_view name) const {
    for (const auto& header : headers_) {
        if (CaseInsensitiveEqual(header.name, name)) {
            return true;
        }
    }
    return false;
}

std::string_view HttpRequest::ContentType() const {
    return GetHeader("Content-Type");
}

std::uint64_t HttpRequest::ContentLength() const {
    const auto value = GetHeader("Content-Length");
    if (value.empty()) {
        return 0;
    }
    std::uint64_t result = 0;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}

std::string_view HttpRequest::QueryParam(std::string_view name) const {
    const auto found = FindQueryParam(name);
    return found.value_or(std::string_view{});
}

bool HttpRequest::HasQueryParam(std::string_view name) const {
    return FindQueryParam(name).has_value();
}

std::string HttpRequest::QueryParamDecoded(std::string_view name) const {
    const auto found = FindQueryParam(name);
    if (!found) {
        return {};
    }
    return PercentDecode(*found, true);
}

std::string_view HttpRequest::Param(std::string_view name) const {
    const auto found = FindParam(name);
    return found.value_or(std::string_view{});
}

bool HttpRequest::HasParam(std::string_view name) const {
    return FindParam(name).has_value();
}

std::string HttpRequest::ParamDecoded(std::string_view name) const {
    const auto found = FindParam(name);
    if (!found) {
        return {};
    }
    return PercentDecode(*found, false);
}

std::string_view HttpRequest::Cookie(std::string_view name) const {
    const auto cookie_header = GetHeader("Cookie");
    if (cookie_header.empty()) {
        return {};
    }

    std::string_view remaining = cookie_header;
    while (!remaining.empty()) {
        const auto sep = remaining.find(';');
        auto part = remaining.substr(0, sep);
        part = TrimAsciiWhitespace(part);

        const auto eq = part.find('=');
        auto key = TrimAsciiWhitespace(part.substr(0, eq));
        auto value = eq == std::string_view::npos
            ? std::string_view{}
            : TrimAsciiWhitespace(part.substr(eq + 1));
        if (key == name) {
            return value;
        }

        if (sep == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(sep + 1);
    }
    return {};
}

bool HttpRequest::HasCookie(std::string_view name) const {
    return !Cookie(name).empty();
}

std::string HttpRequest::CookieDecoded(std::string_view name) const {
    const auto value = Cookie(name);
    if (value.empty()) {
        return {};
    }
    return PercentDecode(value, false);
}

void HttpRequest::AddHeader(std::string name, std::string value) {
    headers_.push_back({std::move(name), std::move(value)});
}

void HttpRequest::ReserveHeaders(std::size_t count) {
    headers_.reserve(count);
}

void HttpRequest::ReserveBody(std::size_t size) {
    body.reserve(size);
}

const std::vector<HttpRequest::Header>& HttpRequest::headers() const {
    return headers_;
}

void HttpRequest::SetParams(
    std::vector<std::pair<std::string_view, std::string_view>> params) {
    params_ = std::move(params);
}

void HttpRequest::ClearParams() {
    params_.clear();
}

void HttpRequest::Clear() {
    method = HttpMethod::kUnknown;
    path.clear();
    query.clear();
    body.clear();
    keep_alive = true;
    request_id.clear();
    headers_.clear();
    params_.clear();
}

std::optional<std::string_view> HttpRequest::FindQueryParam(
    std::string_view name) const {
    if (query.empty()) {
        return std::nullopt;
    }

    std::string_view remaining = query;
    while (!remaining.empty()) {
        const auto amp = remaining.find('&');
        const auto part = remaining.substr(0, amp);
        const auto eq = part.find('=');

        std::string_view key;
        std::string_view value;
        if (eq == std::string_view::npos) {
            key = part;
        } else {
            key = part.substr(0, eq);
            value = part.substr(eq + 1);
        }

        if (key == name) {
            return value;
        }

        if (amp == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(amp + 1);
    }

    return std::nullopt;
}

std::optional<std::string_view> HttpRequest::FindParam(
    std::string_view name) const {
    for (const auto& [key, value] : params_) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

bool HttpRequest::CaseInsensitiveEqual(std::string_view lhs,
                                       std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (ToLower(lhs[i]) != ToLower(rhs[i])) {
            return false;
        }
    }
    return true;
}

char HttpRequest::ToLower(char value) {
    return (value >= 'A' && value <= 'Z')
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

int HttpRequest::HexValue(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string HttpRequest::PercentDecode(std::string_view value,
                                       bool plus_as_space) {
    std::string decoded;
    decoded.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (plus_as_space && ch == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (ch == '%' && i + 2 < value.size()) {
            const int hi = HexValue(value[i + 1]);
            const int lo = HexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(ch);
    }

    return decoded;
}

std::string_view HttpRequest::TrimAsciiWhitespace(std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace iouring::http
