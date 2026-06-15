#pragma once

#include <string>
#include <string_view>

namespace iouring::media {

std::string UrlDecode(std::string_view text);
std::string UrlEncode(std::string_view text);
std::string ResolveUrl(std::string_view base, std::string_view value);
std::string ProxyHlsUrl(std::string_view remote_url);
std::string RewriteHlsManifest(std::string_view text, std::string_view base_url);
std::string ContentTypeForUrl(std::string_view url);

} // namespace iouring::media
