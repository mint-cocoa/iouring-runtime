# API Reference

The default installable package exports one public target:

- `iouring_runtime::Runtime`

Stable public headers are grouped under:

- `iouring_runtime/core/...`

Main public types:

- `iouring_runtime::core::ring::IoRing`
- `iouring_runtime::core::io::Listener`
- `iouring_runtime::core::io::Session`
- `iouring_runtime::core::buffer::SendBuffer`
- `iouring_runtime::core::buffer::RecvBuffer`
- `iouring_runtime::core::buffer::SendQueue`
- `iouring_runtime::core::job::GlobalQueue`
- `iouring_runtime::core::job::JobQueue`
- `iouring_runtime::core::job::JobTimer`

The default runtime target does not export higher-level protocol or
application layers.

When `BUILD_WEB=ON`, an additional package is produced:

- `iouring_runtime_web::RuntimeWeb`
- public headers under `iouring_runtime/web/...`

Main web types:

- `iouring_runtime::web::WebServer`
- `iouring_runtime::web::WebServerConfig`
- `iouring_runtime::web::Router`
- `iouring_runtime::web::HttpRequest`
- `iouring_runtime::web::HttpResponse`
- `iouring_runtime::web::HttpSession`
- `iouring_runtime::web::HttpHandler`
- `iouring_runtime::web::HttpStreamHandler`
- `iouring_runtime::web::HttpMiddleware`

When `BUILD_PROXY=ON`, an additional package is produced:

- `iouring_runtime_proxy::RuntimeProxy`
- public headers under `iouring_runtime/proxy/...`

Main proxy type:

- `iouring_runtime::proxy::TcpProxyServer`

When `BUILD_GAME=ON`, an additional package is produced:

- `iouring_runtime_game::RuntimeGame`
- public headers under `iouring_runtime/game/...`

Main game packet types:

- `iouring_runtime::game::PacketSession`
- `iouring_runtime::game::PlayerRegistry`
- `iouring_runtime::game::PlayerRecord`
- `iouring_runtime::game::RoomLike`
- `iouring_runtime::game::Room`
- `iouring_runtime::game::RoomManager`
- `iouring_runtime::game::RoomManager::RoomInfo`
- `iouring_runtime::game::PlayerState`
- `iouring_runtime::game::PacketBuilder`
- `iouring_runtime::game::PacketId`

Observability helpers are exposed under:

- `iouring_runtime/observability/...`
