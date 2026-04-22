# iouring-runtime

`iouring-runtime` is a runtime-only repository built around three goals:

- expose a reusable `io_uring` execution substrate
- keep protocol and application concerns outside the runtime
- validate lifecycle and shutdown behavior with focused tests and examples

This repository intentionally does not include HTTP, WebSocket, storage, or
game-server layers.

## Scope

The runtime surface includes:

- `IoRing`
- `Listener`
- `Session`
- send/recv buffers
- send queue and backpressure mechanics
- job queue and timers

The runtime surface intentionally excludes:

- HTTP parsing and routing
- WebSocket framing or upgrade
- packet schemas
- storage drivers
- domain/application logic

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Package Target

```cmake
find_package(iouring_runtime CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE iouring_runtime::Runtime)
```

Public headers are available under:

- `#include <iouring_runtime/core/...>`

The C++ namespaces and public header surface now line up around
`iouring_runtime::core` and `iouring_runtime/core/...`.

Install locally with:

```bash
cmake --install build --prefix /tmp/iouring-runtime-install
```

## Examples

- `examples/runtime/core_echo/`
- `examples/runtime/core_idle_echo/`

Build examples with:

```bash
cmake -S . -B build-examples \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-examples --target core_echo core_idle_echo -j$(nproc)
```

## Tests

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_TESTS=ON
cmake --build build-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

## Docs

- Getting started: `docs/getting-started.md`
- API reference: `docs/api-reference.md`
- Runtime guide: `docs/runtime-guide.md`
- Runtime architecture: `docs/runtime-architecture.md`
- Runtime plan: `docs/plans/2026-04-22-io-uring-runtime-plan.md`
- Contributing: `CONTRIBUTING.md`
- Security: `SECURITY.md`
- Roadmap: `ROADMAP.md`
