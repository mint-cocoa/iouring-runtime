#pragma once

#include <iouring_runtime/proxy/AcmeHttpChallengeServer.h>
#include <iouring_runtime/proxy/TcpProxyServer.h>

namespace iouring_runtime::examples::tcp_reverse_proxy {

struct AppConfig {
    proxy::TcpProxyConfig stream;
    proxy::AcmeHttpChallengeConfig acme_http;
};

void ConfigureLoggingFromEnv();

AppConfig LoadConfigFromEnv();

} // namespace iouring_runtime::examples::tcp_reverse_proxy
