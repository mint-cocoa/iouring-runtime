#pragma once

#include <cstdint>
#include <string_view>

namespace iouring::http {

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

std::string_view HttpStatusToString(HttpStatus status);

} // namespace iouring::http
