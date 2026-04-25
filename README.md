# iouring-runtime

`iouring-runtime` is a runtime-only repository built around three goals:

- expose a reusable `io_uring` execution substrate
- keep protocol and application concerns outside the runtime
- validate lifecycle and shutdown behavior with focused tests and examples

This repository intentionally does not include HTTP, WebSocket, storage, or
game-server layers.

An optional HTTP module can be built from the same source tree, but it is a
separate package and target:

- `iouring_runtime_web::RuntimeWeb`
- public headers under `iouring_runtime/web/...`
- enabled only with `-DBUILD_WEB=ON`

An optional TCP proxy module can also be built from the same source tree:

- `iouring_runtime_proxy::RuntimeProxy`
- public headers under `iouring_runtime/proxy/...`
- enabled only with `-DBUILD_PROXY=ON`
- can optionally terminate downstream TLS with OpenSSL

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

Optional web module install:

```bash
cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DBUILD_WEB=ON
cmake --build build-web -j$(nproc)
cmake --install build-web --prefix /tmp/iouring-runtime-install
```

Optional proxy module install:

```bash
cmake -S . -B build-proxy -DCMAKE_BUILD_TYPE=Release -DBUILD_PROXY=ON
cmake --build build-proxy -j$(nproc)
cmake --install build-proxy --prefix /tmp/iouring-runtime-install
```

## Examples

- `examples/runtime/core_echo/`
- `examples/runtime/core_idle_echo/`
- `examples/web/dropapp/`
- `examples/proxy/tcp_reverse_proxy/`
  deployment assets: `examples/proxy/tcp_reverse_proxy/deploy/`

Build examples with:

```bash
cmake -S . -B build-examples \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-examples --target core_echo core_idle_echo -j$(nproc)
```

Build and run the `dropapp` web example:

```bash
cmake -S . -B build-dropapp \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-dropapp --target dropapp -j$(nproc)
DROPAPP_ROOT=/tmp/dropapp DROPAPP_PORT=3000 \
  DROPAPP_STATIC_ROOT=examples/web/dropapp/static \
  ./build-dropapp/bin/dropapp
```

The root `Dockerfile` builds the `dropapp` image. On `main`, the
`dropapp image` workflow pushes `ghcr.io/mint-cocoa/dropapp:${GITHUB_SHA}` and
updates `apps/dropapp/values.yaml` in `mint-cocoa/home-k8s-gitops`.

Build and run the `webhook_inbox` web example:

```bash
cmake -S . -B build-webhook-inbox \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-webhook-inbox --target webhook_inbox -j$(nproc)
WEBHOOK_INBOX_ROOT=/tmp/webhook-inbox WEBHOOK_INBOX_PORT=3000 \
  WEBHOOK_INBOX_STATIC_ROOT=examples/web/webhook_inbox/static \
  ./build-webhook-inbox/bin/webhook_inbox
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
- Proxy guide: `docs/proxy-guide.md`
- Runtime architecture: `docs/runtime-architecture.md`
- Web benchmarking: `docs/web-benchmarking.md`
- Cross-machine benchmark guide: `docs/benchmarking-on-other-machines.md`
- Runtime plan: `docs/plans/2026-04-22-io-uring-runtime-plan.md`
- Contributing: `CONTRIBUTING.md`
- Security: `SECURITY.md`
- Roadmap: `ROADMAP.md`
