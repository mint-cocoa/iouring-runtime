#include "DownstreamTlsContext.h"

#include <openssl/err.h>

#include <array>

#include <iouring_runtime/observability/Logging.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kTls;
}

namespace iouring_runtime::proxy::detail {

void LogOpenSslErrors(std::string_view prefix) {
    bool had_error = false;
    for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
        had_error = true;
        std::array<char, 256> message{};
        ERR_error_string_n(code, message.data(), message.size());
        obs::LogError(kLogCategory, "{}: {}", prefix, message.data());
    }
    if (!had_error) {
        obs::LogError(kLogCategory, "{}: no OpenSSL error details available", prefix);
    }
}

std::shared_ptr<DownstreamTlsContext> BuildDownstreamTlsContext(
    const TcpProxyConfig::DownstreamTlsOptions& options) {
    if (!options.Enabled()) {
        return {};
    }

    if (options.certificate_chain_file.empty() ||
        options.private_key_file.empty()) {
        obs::LogError(kLogCategory,
            "TcpProxyServer: downstream TLS requires both certificate_chain_file and private_key_file");
        return {};
    }

    if (OPENSSL_init_ssl(0, nullptr) != 1) {
        LogOpenSslErrors("TcpProxyServer: OPENSSL_init_ssl failed");
        return {};
    }

    auto ctx = SslCtxHandle(SSL_CTX_new(TLS_server_method()), &SSL_CTX_free);
    if (!ctx) {
        LogOpenSslErrors("TcpProxyServer: SSL_CTX_new failed");
        return {};
    }

    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx.get(),
                     SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                         SSL_MODE_ENABLE_PARTIAL_WRITE);

    if (SSL_CTX_use_certificate_chain_file(
            ctx.get(), options.certificate_chain_file.c_str()) != 1) {
        LogOpenSslErrors(
            "TcpProxyServer: failed to load downstream TLS certificate chain");
        return {};
    }

    if (SSL_CTX_use_PrivateKey_file(
            ctx.get(), options.private_key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        LogOpenSslErrors(
            "TcpProxyServer: failed to load downstream TLS private key");
        return {};
    }

    if (SSL_CTX_check_private_key(ctx.get()) != 1) {
        LogOpenSslErrors(
            "TcpProxyServer: downstream TLS private key does not match certificate");
        return {};
    }

    auto tls_context = std::make_shared<DownstreamTlsContext>();
    tls_context->certificate_chain_file = options.certificate_chain_file;
    tls_context->private_key_file = options.private_key_file;
    tls_context->ctx = std::move(ctx);
    return tls_context;
}

} // namespace iouring_runtime::proxy::detail
