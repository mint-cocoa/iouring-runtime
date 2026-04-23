#pragma once

#include <cstdint>
#include <string_view>

namespace iouring_runtime::web {

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

inline HttpMethod HttpMethodFromLlhttp(int method) {
    switch (method) {
        case 0:  return HttpMethod::kDelete;
        case 1:  return HttpMethod::kGet;
        case 2:  return HttpMethod::kHead;
        case 3:  return HttpMethod::kPost;
        case 4:  return HttpMethod::kPut;
        case 6:  return HttpMethod::kOptions;
        case 28: return HttpMethod::kPatch;
        default: return HttpMethod::kUnknown;
    }
}

inline std::string_view HttpMethodToString(HttpMethod method) {
    switch (method) {
        case HttpMethod::kGet: return "GET";
        case HttpMethod::kHead: return "HEAD";
        case HttpMethod::kPost: return "POST";
        case HttpMethod::kPut: return "PUT";
        case HttpMethod::kDelete: return "DELETE";
        case HttpMethod::kOptions: return "OPTIONS";
        case HttpMethod::kPatch: return "PATCH";
        case HttpMethod::kUnknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

} // namespace iouring_runtime::web
