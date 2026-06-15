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

Build every optional module:

```bash
cmake -S . -B build-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_STREAM=ON \
  -DBUILD_GAME=ON \
  -DBUILD_MEDIA=ON
cmake --build build-all -j$(nproc)
```

Run tests:

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_STREAM=ON \
  -DBUILD_GAME=ON \
  -DBUILD_TESTS=ON
cmake --build build-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

## Pick A Module

| Use case | Link target | Public headers |
| --- | --- | --- |
| Custom TCP server | `iouring::runtime` | `<iouring/core/...>` |
| HTTP app | `iouring::http` | `<iouring/http/...>` |
| TCP reverse proxy | `iouring::stream` | `<iouring/stream/...>` |
| Packet game server | `iouring::game` | `<iouring/game/...>` |
| HLS/media helpers | `iouring::media` | `<iouring/media/...>` |
| Logging/profiling helpers | `iouring::observability` | `<iouring/observability/...>` |

Example:

```cmake
target_link_libraries(my_app PRIVATE
    iouring::http
)
```

```cpp
#include <iouring/http/WebServer.h>
#include <iouring/http/HttpResponse.h>
```

## Examples

Runnable examples and demo apps live in the separate
`iouring-runtime-examples` repository. Build and install this runtime first,
then configure the examples with `CMAKE_PREFIX_PATH`:

```bash
cmake -S . -B build-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_STREAM=ON \
  -DBUILD_GAME=ON \
  -DBUILD_MEDIA=ON
cmake --build build-all -j$(nproc)
cmake --install build-all --prefix /tmp/iouring-install

cmake -S ../iouring-runtime-examples -B ../iouring-runtime-examples/build \
  -DCMAKE_PREFIX_PATH=/tmp/iouring-install
cmake --build ../iouring-runtime-examples/build -j$(nproc)
```

## Install And Consume

```bash
cmake --install build-all --prefix /tmp/iouring-runtime-install
```

Core consumer:

```cmake
find_package(iouring CONFIG REQUIRED)

target_link_libraries(my_echo PRIVATE
    iouring::runtime
)
```

Web consumer:

```cmake
find_package(iouring CONFIG REQUIRED)

target_link_libraries(my_web_app PRIVATE
    iouring::http
)
```

## Repository Map

- `include/iouring/`: public headers grouped by layer and module
- `src/os/`, `src/event/`, `src/net/`: core runtime implementation
- `src/http/`, `src/stream/`, `src/game/`, `src/media/`, `src/observability/`: optional modules
- `benchmarks/`: runtime benchmark servers and wrk helpers
- `tests/`: focused runtime and module tests
- `tools/`: sanitizer and maintenance helpers
- `deploy/`: home-lab Kubernetes manifests

## Docs

- `docs/getting-started.md`: fastest path from clone to building the runtime
- `docs/usage-examples.md`: copyable CMake and C++ usage snippets
- `docs/api-reference.md`: target, header, and public type reference
- `docs/runtime-guide.md`: core TCP runtime walkthrough
- `docs/proxy-guide.md`: TCP proxy configuration and deployment
- `docs/web-benchmarking.md`: `wrk` benchmark scripts
- `docs/benchmarking-on-other-machines.md`: benchmark setup on another host
- `docs/runtime-architecture.md`: implementation notes for maintainers
