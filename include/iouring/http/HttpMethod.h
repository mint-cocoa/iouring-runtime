#pragma once

#include <cstdint>
#include <string_view>

namespace iouring::http {

enum class HttpMethod : std::uint8_t {
    kGet,
    kHead,
    kPost,
    kPut,
    kDelete,
    kOptions,
    kPatch,
    kUnknown,
};

HttpMethod HttpMethodFromLlhttp(int method);

std::string_view HttpMethodToString(HttpMethod method);

} // namespace iouring::http
