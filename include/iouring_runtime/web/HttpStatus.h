#pragma once

#include <cstdint>
#include <string_view>

namespace iouring_runtime::web {

enum class HttpStatus : std::uint16_t {
    kOk = 200,
    kCreated = 201,
    kNoContent = 204,
    kMovedPermanently = 301,
    kFound = 302,
    kNotModified = 304,
    kBadRequest = 400,
    kUnauthorized = 401,
    kForbidden = 403,
    kNotFound = 404,
    kMethodNotAllowed = 405,
    kRequestTimeout = 408,
    kPayloadTooLarge = 413,
    kUriTooLong = 414,
    kRequestHeaderFieldsTooLarge = 431,
    kInternalServerError = 500,
    kNotImplemented = 501,
    kBadGateway = 502,
    kServiceUnavailable = 503,
};

inline std::string_view HttpStatusToString(HttpStatus status) {
    switch (status) {
        case HttpStatus::kOk: return "OK";
        case HttpStatus::kCreated: return "Created";
        case HttpStatus::kNoContent: return "No Content";
        case HttpStatus::kMovedPermanently: return "Moved Permanently";
        case HttpStatus::kFound: return "Found";
        case HttpStatus::kNotModified: return "Not Modified";
        case HttpStatus::kBadRequest: return "Bad Request";
        case HttpStatus::kUnauthorized: return "Unauthorized";
        case HttpStatus::kForbidden: return "Forbidden";
        case HttpStatus::kNotFound: return "Not Found";
        case HttpStatus::kMethodNotAllowed: return "Method Not Allowed";
        case HttpStatus::kRequestTimeout: return "Request Timeout";
        case HttpStatus::kPayloadTooLarge: return "Payload Too Large";
        case HttpStatus::kUriTooLong: return "URI Too Long";
        case HttpStatus::kRequestHeaderFieldsTooLarge:
            return "Request Header Fields Too Large";
        case HttpStatus::kInternalServerError: return "Internal Server Error";
        case HttpStatus::kNotImplemented: return "Not Implemented";
        case HttpStatus::kBadGateway: return "Bad Gateway";
        case HttpStatus::kServiceUnavailable: return "Service Unavailable";
    }
    return "Unknown";
}

} // namespace iouring_runtime::web
