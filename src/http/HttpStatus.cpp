#include <iouring/http/HttpStatus.h>

namespace iouring::http {

std::string_view HttpStatusToString(HttpStatus status) {
    switch (status) {
    case HttpStatus::kOk:
        return "OK";
    case HttpStatus::kCreated:
        return "Created";
    case HttpStatus::kNoContent:
        return "No Content";
    case HttpStatus::kMovedPermanently:
        return "Moved Permanently";
    case HttpStatus::kFound:
        return "Found";
    case HttpStatus::kNotModified:
        return "Not Modified";
    case HttpStatus::kBadRequest:
        return "Bad Request";
    case HttpStatus::kUnauthorized:
        return "Unauthorized";
    case HttpStatus::kForbidden:
        return "Forbidden";
    case HttpStatus::kNotFound:
        return "Not Found";
    case HttpStatus::kMethodNotAllowed:
        return "Method Not Allowed";
    case HttpStatus::kRequestTimeout:
        return "Request Timeout";
    case HttpStatus::kPayloadTooLarge:
        return "Payload Too Large";
    case HttpStatus::kUriTooLong:
        return "URI Too Long";
    case HttpStatus::kRequestHeaderFieldsTooLarge:
        return "Request Header Fields Too Large";
    case HttpStatus::kInternalServerError:
        return "Internal Server Error";
    case HttpStatus::kNotImplemented:
        return "Not Implemented";
    case HttpStatus::kBadGateway:
        return "Bad Gateway";
    case HttpStatus::kServiceUnavailable:
        return "Service Unavailable";
    }
    return "Unknown";
}

} // namespace iouring::http
