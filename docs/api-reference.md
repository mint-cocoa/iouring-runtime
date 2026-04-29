# API Reference

This page lists the public targets, headers, and primary types. For copyable
usage snippets, see `docs/usage-examples.md`.

## Target And Header Rule

Link targets are module boundaries. Include headers are per-source-file API
choices.

```text
Link:    one module target
Include: only the public headers this file uses
```

## Core Runtime

Build option: enabled by default

Target:

- `iouring_runtime::Runtime`

Headers:

- `<iouring_runtime/core/IoRing.h>`
- `<iouring_runtime/core/Listener.h>`
- `<iouring_runtime/core/Session.h>`
- `<iouring_runtime/core/SendBuffer.h>`
- `<iouring_runtime/core/RecvBuffer.h>`
- `<iouring_runtime/core/SendQueue.h>`
- `<iouring_runtime/core/GlobalQueue.h>`
- `<iouring_runtime/core/JobQueue.h>`
- `<iouring_runtime/core/JobTimer.h>`
- `<iouring_runtime/core/Types.h>`

Primary types:

- `iouring_runtime::core::ring::IoRing`
- `iouring_runtime::core::ring::IoRingConfig`
- `iouring_runtime::core::io::Listener`
- `iouring_runtime::core::io::Session`
- `iouring_runtime::core::io::SessionFactory`
- `iouring_runtime::core::buffer::BufferPool`
- `iouring_runtime::core::buffer::SendBuffer`
- `iouring_runtime::core::buffer::RecvBuffer`
- `iouring_runtime::core::buffer::SendQueue`
- `iouring_runtime::core::job::GlobalQueue`
- `iouring_runtime::core::job::JobQueue`
- `iouring_runtime::core::job::JobTimer`
- `iouring_runtime::core::Address`
- `iouring_runtime::core::SessionId`
- `iouring_runtime::core::ContextId`

Example:

```cmake
target_link_libraries(my_echo PRIVATE iouring_runtime::Runtime)
```

```cpp
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/Session.h>
```

## Web Module

Build option: `-DBUILD_WEB=ON`

Target:

- `iouring_runtime_web::RuntimeWeb`

Headers:

- `<iouring_runtime/web/WebServer.h>`
- `<iouring_runtime/web/Router.h>`
- `<iouring_runtime/web/HttpRequest.h>`
- `<iouring_runtime/web/HttpResponse.h>`
- `<iouring_runtime/web/HttpStatus.h>`
- `<iouring_runtime/web/HttpMethod.h>`
- `<iouring_runtime/web/HttpParser.h>`
- `<iouring_runtime/web/HttpSession.h>`
- `<iouring_runtime/web/RadixTree.h>`

Primary types:

- `iouring_runtime::web::WebServer`
- `iouring_runtime::web::WebServerConfig`
- `iouring_runtime::web::RequestContext`
- `iouring_runtime::web::Router`
- `iouring_runtime::web::HttpRequest`
- `iouring_runtime::web::HttpResponse`
- `iouring_runtime::web::HttpStatus`
- `iouring_runtime::web::HttpMethod`
- `iouring_runtime::web::HttpHandler`
- `iouring_runtime::web::HttpStreamHandler`
- `iouring_runtime::web::HttpMiddleware`

Example:

```cmake
target_link_libraries(my_web_app PRIVATE iouring_runtime_web::RuntimeWeb)
```

```cpp
#include <iouring_runtime/web/WebServer.h>
#include <iouring_runtime/web/HttpResponse.h>
```

## Proxy Module

Build option: `-DBUILD_PROXY=ON`

Target:

- `iouring_runtime_proxy::RuntimeProxy`

Headers:

- `<iouring_runtime/proxy/TcpProxyServer.h>`
- `<iouring_runtime/proxy/AcmeHttpChallengeServer.h>`

Primary types:

- `iouring_runtime::proxy::TcpProxyConfig`
- `iouring_runtime::proxy::TcpProxyServer`
- `iouring_runtime::proxy::AcmeHttpChallengeConfig`
- `iouring_runtime::proxy::AcmeHttpChallengeServer`

Example:

```cmake
target_link_libraries(my_proxy PRIVATE iouring_runtime_proxy::RuntimeProxy)
```

```cpp
#include <iouring_runtime/proxy/TcpProxyServer.h>
```

## Game Module

Build option: `-DBUILD_GAME=ON`

Target:

- `iouring_runtime_game::RuntimeGame`

Headers:

- `<iouring_runtime/game/PacketSession.h>`
- `<iouring_runtime/game/PacketBuilder.h>`
- `<iouring_runtime/game/PlayerRegistry.h>`
- `<iouring_runtime/game/Room.h>`
- `<iouring_runtime/game/RoomManager.h>`
- `<iouring_runtime/game/Types.h>`

Primary types:

- `iouring_runtime::game::PacketSession`
- `iouring_runtime::game::PacketBuilder`
- `iouring_runtime::game::PacketId`
- `iouring_runtime::game::PlayerRegistry`
- `iouring_runtime::game::PlayerRecord`
- `iouring_runtime::game::PlayerState`
- `iouring_runtime::game::Room`
- `iouring_runtime::game::RoomLike`
- `iouring_runtime::game::RoomManager`
- `iouring_runtime::game::RoomManager::RoomInfo`

Example:

```cmake
target_link_libraries(my_game PRIVATE iouring_runtime_game::RuntimeGame)
```

```cpp
#include <iouring_runtime/game/PacketSession.h>
#include <iouring_runtime/game/RoomManager.h>
```

## Media Helpers

Build option: enabled by default

Target:

- `iouring_runtime::RuntimeMedia`

Headers:

- `<iouring_runtime/media/Hls.h>`

Use this target for HLS manifest rewriting, URL decoding, and HLS content type
helpers.

## Observability Helpers

Build option: enabled by default

Target:

- `iouring_runtime::RuntimeObservability`

Headers:

- `<iouring_runtime/observability/Logging.h>`
- `<iouring_runtime/observability/Profiler.h>`

Use this target for logging helpers shared by runtime modules and examples.

## Installed Packages

The core package is always produced:

```cmake
find_package(iouring_runtime CONFIG REQUIRED)
```

Optional packages are produced only when their build option is enabled:

```cmake
find_package(iouring_runtime_web CONFIG REQUIRED)
find_package(iouring_runtime_proxy CONFIG REQUIRED)
find_package(iouring_runtime_game CONFIG REQUIRED)
```

The optional packages depend on `iouring_runtime`, so consumers usually only
need to link the highest-level module target they use.
