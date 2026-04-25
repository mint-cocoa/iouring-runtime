#pragma once

#include <iouring_runtime/proxy/TcpProxyServer.h>

#include <openssl/ssl.h>

#include <memory>
#include <string>
#include <string_view>

namespace iouring_runtime::proxy::detail {

using SslCtxHandle = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using SslHandle = std::unique_ptr<SSL, decltype(&SSL_free)>;

struct DownstreamTlsContext {
    std::string certificate_chain_file;
    std::string private_key_file;
    SslCtxHandle ctx{nullptr, &SSL_CTX_free};
};

void LogOpenSslErrors(std::string_view prefix);

std::shared_ptr<DownstreamTlsContext> BuildDownstreamTlsContext(
    const TcpProxyConfig::DownstreamTlsOptions& options);

} // namespace iouring_runtime::proxy::detail
