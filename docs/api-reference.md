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

## Core runtime

Build option: enabled by default

Target:

- `iouring::runtime`

Headers:

- `<iouring/event/IoRing.h>`
- `<iouring/net/Listener.h>`
- `<iouring/net/Session.h>`
- `<iouring/core/SendBuffer.h>`
- `<iouring/core/RecvBuffer.h>`
- `<iouring/core/SendQueue.h>`
- `<iouring/event/GlobalQueue.h>`
- `<iouring/event/JobQueue.h>`
- `<iouring/event/JobTimer.h>`
- `<iouring/core/Types.h>`

Primary types:

- `iouring::event::IoRing`
- `iouring::event::IoRingConfig`
- `iouring::net::Listener`
- `iouring::net::Session`
- `iouring::net::SessionFactory`
- `iouring::core::buffer::BufferPool`
- `iouring::core::buffer::SendBuffer`
- `iouring::core::buffer::RecvBuffer`
- `iouring::core::buffer::SendQueue`
- `iouring::event::GlobalQueue`
- `iouring::event::JobQueue`
- `iouring::event::JobTimer`
- `iouring::core::Address`
- `iouring::core::SessionId`
- `iouring::core::ContextId`

Example:

```cmake
target_link_libraries(my_echo PRIVATE iouring::runtime)
```

```cpp
#include <iouring/event/IoRing.h>
#include <iouring/net/Listener.h>
#include <iouring/net/Session.h>
```

## Web Module

Build option: `-DBUILD_HTTP=ON`

Target:

- `iouring::http`

Headers:

- `<iouring/http/WebServer.h>`
- `<iouring/http/Router.h>`
- `<iouring/http/HttpRequest.h>`
- `<iouring/http/HttpResponse.h>`
- `<iouring/http/HttpStatus.h>`
- `<iouring/http/HttpMethod.h>`
- `<iouring/http/HttpParser.h>`
- `<iouring/http/HttpSession.h>`
- `<iouring/http/RadixTree.h>`

Primary types:

- `iouring::http::WebServer`
- `iouring::http::WebServerConfig`
- `iouring::http::RequestContext`
- `iouring::http::Router`
- `iouring::http::HttpRequest`
- `iouring::http::HttpResponse`
- `iouring::http::HttpStatus`
- `iouring::http::HttpMethod`
- `iouring::http::HttpHandler`
- `iouring::http::HttpStreamHandler`
- `iouring::http::HttpMiddleware`

Example:

```cmake
target_link_libraries(my_web_app PRIVATE iouring::http)
```

```cpp
#include <iouring/http/WebServer.h>
#include <iouring/http/HttpResponse.h>
```

## Proxy Module

Build option: `-DBUILD_STREAM=ON`

Target:

- `iouring::stream`

Headers:

- `<iouring/stream/TcpProxyServer.h>`
- `<iouring/stream/AcmeHttpChallengeServer.h>`

Primary types:

- `iouring::stream::TcpProxyConfig`
- `iouring::stream::TcpProxyServer`
- `iouring::stream::AcmeHttpChallengeConfig`
- `iouring::stream::AcmeHttpChallengeServer`

Example:

```cmake
target_link_libraries(my_proxy PRIVATE iouring::stream)
```

```cpp
#include <iouring/stream/TcpProxyServer.h>
```

## Game Module

Build option: `-DBUILD_GAME=ON`

Target:

- `iouring::game`

Headers:

- `<iouring/game/PacketSession.h>`
- `<iouring/game/PacketBuilder.h>`
- `<iouring/game/PlayerRegistry.h>`
- `<iouring/game/Room.h>`
- `<iouring/game/RoomManager.h>`
- `<iouring/game/Types.h>`

Primary types:

- `iouring::game::PacketSession`
- `iouring::game::PacketBuilder`
- `iouring::game::PacketId`
- `iouring::game::PlayerRegistry`
- `iouring::game::PlayerRecord`
- `iouring::game::PlayerState`
- `iouring::game::Room`
- `iouring::game::RoomLike`
- `iouring::game::RoomManager`
- `iouring::game::RoomManager::RoomInfo`

Example:

```cmake
target_link_libraries(my_game PRIVATE iouring::game)
```

```cpp
#include <iouring/game/PacketSession.h>
#include <iouring/game/RoomManager.h>
```

## Media Helpers

Build option: enabled by default

Target:

- `iouring::media`

Headers:

- `<iouring/media/Hls.h>`

Use this target for HLS manifest rewriting, URL decoding, and HLS content type
helpers.

## Observability Helpers

Build option: enabled by default

Target:

- `iouring::observability`

Headers:

- `<iouring/observability/Logging.h>`
- `<iouring/observability/Profiler.h>`

Use this target for logging helpers shared by runtime modules and examples.

## Installed Packages

The core package is always produced:

```cmake
find_package(iouring CONFIG REQUIRED)
```

Optional packages are produced only when their build option is enabled:

```cmake
find_package(iouring CONFIG REQUIRED)
find_package(iouring_proxy CONFIG REQUIRED)
find_package(iouring CONFIG REQUIRED)
```

The optional packages depend on `iouring`, so consumers usually only
need to link the highest-level module target they use.
