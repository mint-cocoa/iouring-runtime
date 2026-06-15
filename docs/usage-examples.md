# Usage Examples

Applications link a module target, then include the public headers they use.

```text
CMake target: module-level link boundary
C++ include:  per-file public API selection
```

## Core TCP Server

Link:

```cmake
find_package(iouring CONFIG REQUIRED)

add_executable(my_echo src/main.cpp)
target_link_libraries(my_echo PRIVATE
    iouring::runtime
)
target_compile_features(my_echo PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring/core/SendBuffer.h>
#include <iouring/net/Session.h>
#include <iouring/core/Types.h>
#include <iouring/event/Worker.h>
```

Minimal receive handler:

```cpp
class EchoSession final : public iouring::net::Session {
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
iouring::net::SessionFactory factory =
    [](int fd,
       iouring::event::IoRing& ring,
       iouring::core::buffer::BufferPool& pool,
       iouring::core::ContextId)
        -> iouring::net::SessionRef {
    return std::make_shared<EchoSession>(fd, ring, pool);
};

iouring::event::WorkerConfig config;
config.address = {"0.0.0.0", 19090};

iouring::event::Worker worker(config, std::move(factory));
worker.Start();
```

Full example: `iouring-runtime-examples/core/core_echo/`.

## HTTP Server

Link:

```cmake
find_package(iouring CONFIG REQUIRED)

add_executable(my_web_app src/main.cpp)
target_link_libraries(my_web_app PRIVATE
    iouring::http
)
target_compile_features(my_web_app PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring/http/WebServer.h>
#include <iouring/http/HttpResponse.h>
```

Minimal server:

```cpp
int main() {
    iouring::http::WebServerConfig config;
    config.port = 8080;
    config.worker_count = 1;

    iouring::http::WebServer server(config);
    iouring::http::WebServer::InstallStopSignalHandlers();

    server.Get("/", [](iouring::http::RequestContext& ctx) {
        ctx.response.Text("hello").Send();
    });

    server.Get("/healthz", [](iouring::http::RequestContext& ctx) {
        ctx.response.Text("ok").Send();
    });

    server.Start();
    iouring::http::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
}
```

Common request helpers:

```cpp
server.Get("/hello/:name", [](iouring::http::RequestContext& ctx) {
    auto name = ctx.request.ParamDecoded("name");
    auto lang = ctx.request.QueryParamDecoded("lang");
    ctx.response.Json("{\"name\":\"" + name + "\",\"lang\":\"" +
                      std::string(lang) + "\"}").Send();
});
```

Full examples:

- `iouring-runtime-examples/http/hello_http/`
- `iouring-runtime-examples/http/file_store_server/`
- `iouring-runtime-examples/http/dropapp/`
- `iouring-runtime-examples/http/webhook_inbox/`
- `iouring-runtime-examples/http/status_server/`
- `iouring-runtime-examples/http/speedtest_server/`

## Router-Only Tests Or Utilities

Link the web module, but include only router/request/response headers:

```cmake
target_link_libraries(router_tool PRIVATE
    iouring::http
)
```

```cpp
#include <iouring/http/Router.h>
#include <iouring/http/HttpRequest.h>
#include <iouring/http/HttpResponse.h>
```

Reference test: `tests/web/test_router.cpp`.

## TCP Reverse Proxy

Link:

```cmake
find_package(iouring_proxy CONFIG REQUIRED)

add_executable(my_proxy src/main.cpp)
target_link_libraries(my_proxy PRIVATE
    iouring::stream
)
target_compile_features(my_proxy PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring/stream/TcpProxyServer.h>
```

Minimal proxy:

```cpp
int main() {
    iouring::stream::TcpProxyConfig config;
    config.listen_host = "0.0.0.0";
    config.listen_port = 18080;
    config.upstream_host = "127.0.0.1";
    config.upstream_port = 8080;
    config.worker_count = 1;

    iouring::stream::TcpProxyServer server(config);
    server.Start();

    std::this_thread::sleep_for(std::chrono::hours(24));
    server.Stop();
}
```

Full example: `iouring-runtime-examples/stream/tcp_reverse_proxy/`.

## Game Packet Server

Link:

```cmake
find_package(iouring CONFIG REQUIRED)

add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE
    iouring::game
)
target_compile_features(my_game PRIVATE cxx_std_23)
```

Include:

```cpp
#include <iouring/game/PacketSession.h>
#include <iouring/game/PlayerRegistry.h>
#include <iouring/game/RoomManager.h>
```

Typical setup:

```cpp
iouring::event::GlobalQueue global_queue;

auto players = std::make_shared<iouring::game::PlayerRegistry>();
auto rooms = std::make_shared<iouring::game::RoomManager>(global_queue);
```

Full examples:

- `iouring-runtime-examples/game/dungeon_packet_echo/`
- `iouring-runtime-examples/game/dungeon_full_server/`

## Media Helpers

Link:

```cmake
find_package(iouring CONFIG REQUIRED)

target_link_libraries(my_media_tool PRIVATE
    iouring::media
)
```

Include:

```cpp
#include <iouring/media/Hls.h>
```

Reference users:

- `tests/media/test_hls.cpp`
- `iouring-runtime-examples/activity/server/`

## Observability Helpers

Link:

```cmake
find_package(iouring CONFIG REQUIRED)

target_link_libraries(my_server PRIVATE
    iouring::observability
)
```

Include:

```cpp
#include <iouring/observability/Logging.h>
```

Example:

```cpp
iouring::observability::ConfigureLoggingFromEnv("MY_APP_LOG_LEVEL");
```

Reference users:

- `iouring-runtime-examples/http/hello_http/`
- `iouring-runtime-examples/stream/tcp_reverse_proxy/`
- `tests/observability/test_logging.cpp`
