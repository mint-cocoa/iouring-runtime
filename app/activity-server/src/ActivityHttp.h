#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace activity_server {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

std::string Header(const HttpRequest& req, std::string_view name);

} // namespace activity_server
