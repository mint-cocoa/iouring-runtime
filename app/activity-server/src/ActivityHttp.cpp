#include "ActivityHttp.h"

namespace activity_server {

std::string Header(const HttpRequest& req, std::string_view name) {
    const auto it = req.headers.find(std::string(name));
    return it == req.headers.end() ? std::string{} : it->second;
}

} // namespace activity_server
