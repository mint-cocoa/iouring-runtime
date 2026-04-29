#include "TcpReverseProxyConfig.h"

#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/proxy/AcmeHttpChallengeServer.h>
#include <iouring_runtime/proxy/TcpProxyServer.h>

#include <chrono>
#include <thread>

namespace {

namespace example = iouring_runtime::examples::tcp_reverse_proxy;
namespace obs = iouring_runtime::observability;
namespace proxy = iouring_runtime::proxy;

constexpr auto kLogCategory = obs::LogCategory::kProxy;

void RunUntilStop(proxy::TcpProxyServer& server) {
    while (!proxy::TcpProxyServer::StopRequested()) {
        if (proxy::TcpProxyServer::ConsumeReloadRequest() &&
            !server.ReloadDownstreamTlsContext()) {
            obs::LogError(
                kLogCategory,
                "tcp_reverse_proxy: failed to reload downstream TLS context");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace

int main() {
    example::ConfigureLoggingFromEnv();

    const auto config = example::LoadConfigFromEnv();
    proxy::TcpProxyServer server(config.stream);
    proxy::AcmeHttpChallengeServer acme_http(config.acme_http);

    proxy::TcpProxyServer::InstallStopSignalHandlers();
    server.Start();
    acme_http.Start();
    RunUntilStop(server);
    acme_http.Stop();
    server.Stop();
    return 0;
}
