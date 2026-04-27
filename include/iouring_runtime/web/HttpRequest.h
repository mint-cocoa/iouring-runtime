#pragma once

#include <iouring_runtime/web/HttpMethod.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace iouring_runtime::web {

class HttpRequest {
public:
    struct Header {
        std::string name;
        std::string value;
    };

    HttpMethod method = HttpMethod::kUnknown;
    std::string path;
    std::string query;
    std::string body;
    bool keep_alive = true;
    std::string request_id;

    std::string_view GetHeader(std::string_view name) const {
        for (const auto& header : headers_) {
            if (CaseInsensitiveEqual(header.name, name)) {
                return header.value;
            }
        }
        return {};
    }

    bool HasHeader(std::string_view name) const {
        for (const auto& header : headers_) {
            if (CaseInsensitiveEqual(header.name, name)) {
                return true;
            }
        }
        return false;
    }

    std::string_view ContentType() const {
        return GetHeader("Content-Type");
    }

    std::uint64_t ContentLength() const {
        const auto value = GetHeader("Content-Length");
        if (value.empty()) {
            return 0;
        }
        std::uint64_t result = 0;
        std::from_chars(value.data(), value.data() + value.size(), result);
        return result;
    }

    std::string_view QueryParam(std::string_view name) const {
        const auto found = FindQueryParam(name);
        return found.value_or(std::string_view{});
    }

    bool HasQueryParam(std::string_view name) const {
        return FindQueryParam(name).has_value();
    }

    std::string QueryParamDecoded(std::string_view name) const {
        const auto found = FindQueryParam(name);
        if (!found) {
            return {};
        }
        return PercentDecode(*found, true);
    }

    template <typename T>
    std::optional<T> QueryParamAs(std::string_view name) const {
        const auto found = FindQueryParam(name);
        if (!found) {
            return std::nullopt;
        }
        return ConvertValue<T>(*found);
    }

    template <typename T>
    T QueryParam(std::string_view name, T default_value) const {
        auto parsed = QueryParamAs<T>(name);
        return parsed ? *std::move(parsed) : std::move(default_value);
    }

    std::string_view Param(std::string_view name) const {
        const auto found = FindParam(name);
        return found.value_or(std::string_view{});
    }

    bool HasParam(std::string_view name) const {
        return FindParam(name).has_value();
    }

    std::string ParamDecoded(std::string_view name) const {
        const auto found = FindParam(name);
        if (!found) {
            return {};
        }
        return PercentDecode(*found, false);
    }

    std::string_view Cookie(std::string_view name) const {
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

    bool HasCookie(std::string_view name) const {
        return !Cookie(name).empty();
    }

    std::string CookieDecoded(std::string_view name) const {
        const auto value = Cookie(name);
        if (value.empty()) {
            return {};
        }
        return PercentDecode(value, false);
    }

    template <typename T>
    std::optional<T> ParamAs(std::string_view name) const {
        const auto found = FindParam(name);
        if (!found) {
            return std::nullopt;
        }
        return ConvertValue<T>(*found);
    }

    template <typename T>
    T Param(std::string_view name, T default_value) const {
        auto parsed = ParamAs<T>(name);
        return parsed ? *std::move(parsed) : std::move(default_value);
    }

    void AddHeader(std::string name, std::string value) {
        headers_.push_back({std::move(name), std::move(value)});
    }

    void ReserveHeaders(std::size_t count) {
        headers_.reserve(count);
    }

    void ReserveBody(std::size_t size) {
        body.reserve(size);
    }

    const std::vector<Header>& headers() const {
        return headers_;
    }

    void SetParams(std::vector<std::pair<std::string_view, std::string_view>> params) {
        params_ = std::move(params);
    }

    void ClearParams() {
        params_.clear();
    }

    void Clear() {
        method = HttpMethod::kUnknown;
        path.clear();
        query.clear();
        body.clear();
        keep_alive = true;
        request_id.clear();
        headers_.clear();
        params_.clear();
    }

private:
    template <typename>
    struct AlwaysFalse : std::false_type {};

    std::optional<std::string_view> FindQueryParam(std::string_view name) const {
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

    std::optional<std::string_view> FindParam(std::string_view name) const {
        for (const auto& [key, value] : params_) {
            if (key == name) {
                return value;
            }
        }
        return std::nullopt;
    }

    template <typename T>
    static std::optional<T> ConvertValue(std::string_view value) {
        using U = std::remove_cv_t<T>;

        if constexpr (std::is_same_v<U, std::string_view>) {
            return value;
        } else if constexpr (std::is_same_v<U, std::string>) {
            return std::string(value);
        } else if constexpr (std::is_same_v<U, bool>) {
            if (value == "true" || value == "1") {
                return true;
            }
            if (value == "false" || value == "0") {
                return false;
            }
            return std::nullopt;
        } else if constexpr (std::is_integral_v<U> || std::is_floating_point_v<U>) {
            if (value.empty()) {
                return std::nullopt;
            }
            U result{};
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), result);
            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                return std::nullopt;
            }
            return result;
        } else {
            static_assert(AlwaysFalse<T>::value,
                          "HttpRequest values support string, string_view, bool, integral, and floating-point types");
        }
    }

    static bool CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
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

    static char ToLower(char value) {
        return (value >= 'A' && value <= 'Z')
            ? static_cast<char>(value + ('a' - 'A'))
            : value;
    }

    static int HexValue(char value) {
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

    static std::string PercentDecode(std::string_view value, bool plus_as_space) {
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

    static std::string_view TrimAsciiWhitespace(std::string_view value) {
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

    std::vector<Header> headers_;
    std::vector<std::pair<std::string_view, std::string_view>> params_;
};

} // namespace iouring_runtime::web
