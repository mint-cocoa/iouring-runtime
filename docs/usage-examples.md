# Usage Examples

Applications link a module target, then include the public headers they use.

```text
CMake target: module-level link boundary
C++ include:  per-file public API selection
```

## Core TCP Server

Link:

```cmake
find_package(iouring_runtime CONFIG REQUIRED)

add_executable(my_echo src/main.cpp)
target_link_libraries(my_echo PRIVATE
    iouring_runtime::Runtime
)
target_compile_features(my_echo PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Types.h>
#include <iouring_runtime/core/Worker.h>
```

Minimal receive handler:

```cpp
class EchoSession final : public iouring_runtime::core::io::Session {
public:
    using Session::Session;

private:
    void OnRecv(std::span<const std::byte> data) override {
        auto result = Pool().Allocate(static_cast<std::uint32_t>(data.size()));
        if (!result) {
            Disconnect();
            return;
        }

        auto buffer = std::move(*result);
        std::memcpy(buffer->Writable().data(), data.data(), data.size());
        buffer->Commit(static_cast<std::uint32_t>(data.size()));

        if (!Send(std::move(buffer)).has_value()) {
            Disconnect();
        }
    }
};
```

Start the server with a worker:

```cpp
iouring_runtime::core::io::SessionFactory factory =
    [](int fd,
       iouring_runtime::core::ring::IoRing& ring,
       iouring_runtime::core::buffer::BufferPool& pool,
       iouring_runtime::core::ContextId)
        -> iouring_runtime::core::io::SessionRef {
    return std::make_shared<EchoSession>(fd, ring, pool);
};

iouring_runtime::core::io::WorkerConfig config;
config.address = {"0.0.0.0", 19090};

iouring_runtime::core::io::Worker worker(config, std::move(factory));
worker.Start();
```

Full example: `app/examples/runtime/core_echo/`.

## HTTP Server

Link:

```cmake
find_package(iouring_runtime_web CONFIG REQUIRED)

add_executable(my_web_app src/main.cpp)
target_link_libraries(my_web_app PRIVATE
    iouring_runtime_web::RuntimeWeb
)
target_compile_features(my_web_app PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring_runtime/web/WebServer.h>
#include <iouring_runtime/web/HttpResponse.h>
```

Minimal server:

```cpp
int main() {
    iouring_runtime::web::WebServerConfig config;
    config.port = 8080;
    config.worker_count = 1;

    iouring_runtime::web::WebServer server(config);
    iouring_runtime::web::WebServer::InstallStopSignalHandlers();

    server.Get("/", [](iouring_runtime::web::RequestContext& ctx) {
        ctx.response.Text("hello").Send();
    });

    server.Get("/healthz", [](iouring_runtime::web::RequestContext& ctx) {
        ctx.response.Text("ok").Send();
    });

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
}
```

Common request helpers:

```cpp
server.Get("/hello/:name", [](iouring_runtime::web::RequestContext& ctx) {
    auto name = ctx.request.ParamDecoded("name");
    auto lang = ctx.request.QueryParamDecoded("lang");
    ctx.response.Json("{\"name\":\"" + name + "\",\"lang\":\"" +
                      std::string(lang) + "\"}").Send();
});
```

Full examples:

- `app/examples/web/hello_http/`
- `app/examples/web/file_store_server/`
- `app/examples/web/dropapp/`
- `app/examples/web/webhook_inbox/`
- `app/examples/web/status_server/`
- `app/examples/web/speedtest_server/`

## Router-Only Tests Or Utilities

Link the web module, but include only router/request/response headers:

```cmake
target_link_libraries(router_tool PRIVATE
    iouring_runtime_web::RuntimeWeb
)
```

```cpp
#include <iouring_runtime/web/Router.h>
#include <iouring_runtime/web/HttpRequest.h>
#include <iouring_runtime/web/HttpResponse.h>
```

Reference test: `tests/web/test_router.cpp`.

## TCP Reverse Proxy

Link:

```cmake
find_package(iouring_runtime_proxy CONFIG REQUIRED)

add_executable(my_proxy src/main.cpp)
target_link_libraries(my_proxy PRIVATE
    iouring_runtime_proxy::RuntimeProxy
)
target_compile_features(my_proxy PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring_runtime/proxy/TcpProxyServer.h>
```

Minimal proxy:

```cpp
int main() {
    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "0.0.0.0";
    config.listen_port = 18080;
    config.upstream_host = "127.0.0.1";
    config.upstream_port = 8080;
    config.worker_count = 1;

    iouring_runtime::proxy::TcpProxyServer server(config);
    server.Start();

    std::this_thread::sleep_for(std::chrono::hours(24));
    server.Stop();
}
```

Full example: `app/examples/proxy/tcp_reverse_proxy/`.

## Game Packet Server

Link:

```cmake
find_package(iouring_runtime_game CONFIG REQUIRED)

add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE
    iouring_runtime_game::RuntimeGame
)
target_compile_features(my_game PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring_runtime/game/PacketSession.h>
#include <iouring_runtime/game/PlayerRegistry.h>
#include <iouring_runtime/game/RoomManager.h>
```

Typical setup:

```cpp
iouring_runtime::core::job::GlobalQueue global_queue;

auto players = std::make_shared<iouring_runtime::game::PlayerRegistry>();
auto rooms = std::make_shared<iouring_runtime::game::RoomManager>(global_queue);
```

Full examples:

- `app/examples/game/dungeon_packet_echo/`
- `app/examples/game/dungeon_full_server/`

## Media Helpers

Link:

```cmake
find_package(iouring_runtime CONFIG REQUIRED)

target_link_libraries(my_media_tool PRIVATE
    iouring_runtime::RuntimeMedia
)
```

Include:

```cpp
#include <iouring_runtime/media/Hls.h>
```

Reference users:

- `tests/media/test_hls.cpp`
- `app/activity-server/`

## Observability Helpers

Link:

```cmake
find_package(iouring_runtime CONFIG REQUIRED)

target_link_libraries(my_server PRIVATE
    iouring_runtime::RuntimeObservability
)
```

Include:

```cpp
#include <iouring_runtime/observability/Logging.h>
```

Example:

```cpp
iouring_runtime::observability::ConfigureLoggingFromEnv("MY_APP_LOG_LEVEL");
```

Reference users:

- `app/examples/web/hello_http/`
- `app/examples/proxy/tcp_reverse_proxy/`
- `tests/observability/test_logging.cpp`
