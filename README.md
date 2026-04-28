# iouring-runtime

`iouring-runtime` is a C++23 server runtime built on Linux `io_uring`.
The core package stays protocol-agnostic, while HTTP, TCP proxy, and game
packet helpers are provided as optional module targets.

Use it in two steps:

```text
1. Link the module target you need.
2. Include only the public headers used by the source file.
```

## Quick Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Build every optional module and example:

```bash
cmake -S . -B build-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_PROXY=ON \
  -DBUILD_GAME=ON \
  -DBUILD_EXAMPLES=ON
cmake --build build-all -j$(nproc)
```

Run tests:

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_PROXY=ON \
  -DBUILD_GAME=ON \
  -DBUILD_TESTS=ON
cmake --build build-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

## Pick A Module

| Use case | Link target | Public headers |
| --- | --- | --- |
| Custom TCP server | `iouring_runtime::Runtime` | `<iouring_runtime/core/...>` |
| HTTP app | `iouring_runtime_web::RuntimeWeb` | `<iouring_runtime/web/...>` |
| TCP reverse proxy | `iouring_runtime_proxy::RuntimeProxy` | `<iouring_runtime/proxy/...>` |
| Packet game server | `iouring_runtime_game::RuntimeGame` | `<iouring_runtime/game/...>` |
| HLS/media helpers | `iouring_runtime::RuntimeMedia` | `<iouring_runtime/media/...>` |
| Logging/profiling helpers | `iouring_runtime::RuntimeObservability` | `<iouring_runtime/observability/...>` |

Example:

```cmake
target_link_libraries(my_app PRIVATE
    iouring_runtime_web::RuntimeWeb
)
```

```cpp
#include <iouring_runtime/web/WebServer.h>
#include <iouring_runtime/web/HttpResponse.h>
```

## Run The Examples

Core TCP echo:

```bash
cmake -S . -B build-core \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-core --target core_echo -j$(nproc)
CORE_ECHO_PORT=19090 ./build-core/bin/core_echo
```

HTTP hello server:

```bash
cmake -S . -B build-web \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-web --target hello_http -j$(nproc)
HELLO_HTTP_PORT=8080 ./build-web/bin/hello_http
```

TCP reverse proxy:

```bash
cmake -S . -B build-proxy \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_PROXY=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-proxy --target tcp_reverse_proxy -j$(nproc)
TCP_PROXY_LISTEN_PORT=18080 \
TCP_PROXY_UPSTREAM_HOST=127.0.0.1 \
TCP_PROXY_UPSTREAM_PORT=8080 \
./build-proxy/bin/tcp_reverse_proxy
```

Game packet echo:

```bash
cmake -S . -B build-game \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GAME=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-game --target dungeon_packet_echo -j$(nproc)
DUNGEON_PACKET_ECHO_PORT=19110 ./build-game/bin/dungeon_packet_echo
```

## Install And Consume

```bash
cmake --install build-all --prefix /tmp/iouring-runtime-install
```

Core consumer:

```cmake
find_package(iouring_runtime CONFIG REQUIRED)

target_link_libraries(my_echo PRIVATE
    iouring_runtime::Runtime
)
```

Web consumer:

```cmake
find_package(iouring_runtime_web CONFIG REQUIRED)

target_link_libraries(my_web_app PRIVATE
    iouring_runtime_web::RuntimeWeb
)
```

## Repository Map

- `include/iouring_runtime/`: public headers grouped by module
- `src/runtime/`: core runtime implementation
- `src/modules/`: optional web, proxy, game, media, and observability modules
- `app/examples/`: runnable examples for each module
- `app/activity-server/`: C++ Activity backend built on the runtime
- `tests/`: focused runtime and module tests
- `scripts/`: sanitizer and benchmarking helpers
- `deploy/`: home-lab Kubernetes manifests

## Docs

- `docs/getting-started.md`: fastest path from clone to running examples
- `docs/usage-examples.md`: copyable CMake and C++ usage snippets
- `docs/api-reference.md`: target, header, and public type reference
- `docs/runtime-guide.md`: core TCP runtime walkthrough
- `docs/proxy-guide.md`: TCP proxy configuration and deployment
- `docs/web-benchmarking.md`: `wrk` benchmark scripts
- `docs/benchmarking-on-other-machines.md`: benchmark setup on another host
- `docs/runtime-architecture.md`: implementation notes for maintainers
