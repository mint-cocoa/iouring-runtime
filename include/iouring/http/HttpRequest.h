#pragma once

#include <iouring/http/HttpMethod.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace iouring::http {

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

    std::string_view GetHeader(std::string_view name) const;

    bool HasHeader(std::string_view name) const;

    std::string_view ContentType() const;

    std::uint64_t ContentLength() const;

    std::string_view QueryParam(std::string_view name) const;

    bool HasQueryParam(std::string_view name) const;

    std::string QueryParamDecoded(std::string_view name) const;

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

    std::string_view Param(std::string_view name) const;

    bool HasParam(std::string_view name) const;

    std::string ParamDecoded(std::string_view name) const;

    std::string_view Cookie(std::string_view name) const;

    bool HasCookie(std::string_view name) const;

    std::string CookieDecoded(std::string_view name) const;

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

    void AddHeader(std::string name, std::string value);

    void ReserveHeaders(std::size_t count);

    void ReserveBody(std::size_t size);

    const std::vector<Header>& headers() const;

    void SetParams(std::vector<std::pair<std::string_view, std::string_view>> params);

    void ClearParams();

    void Clear();

private:
    template <typename>
    struct AlwaysFalse : std::false_type {};

    std::optional<std::string_view> FindQueryParam(std::string_view name) const;

    std::optional<std::string_view> FindParam(std::string_view name) const;

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

    static bool CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs);

    static char ToLower(char value);

    static int HexValue(char value);

    static std::string PercentDecode(std::string_view value, bool plus_as_space);

    static std::string_view TrimAsciiWhitespace(std::string_view value);

    std::vector<Header> headers_;
    std::vector<std::pair<std::string_view, std::string_view>> params_;
};

} // namespace iouring::http
