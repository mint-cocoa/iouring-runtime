# iouring-runtime

`iouring-runtime` is a runtime-only repository built around three goals:

- expose a reusable `io_uring` execution substrate
- keep protocol and application concerns outside the runtime
- validate lifecycle and shutdown behavior with focused tests and examples

The default package is intentionally limited to the runtime core. Higher-level
HTTP, proxy, and multiplayer packet support live in optional module targets so
the core target stays small and protocol-agnostic.

## Portfolio Snapshot

This repository is also the main portfolio page for two connected tracks:

- **C++ server runtime**
  - Linux `io_uring` runtime core with listener/session lifecycle, send/recv
    buffers, queued writes, backpressure, job queues, timers, and focused
    shutdown tests.
  - Modular server layers under `src/modules`: HTTP routing/static serving,
    TCP reverse proxying with optional TLS/ACME helpers, multiplayer packet
    sessions, observability helpers, and reusable media/HLS utilities.
  - Runnable C++ examples for echo servers, HTTP apps, file hosting,
    speedtest/status pages, webhook capture, TCP proxying, and dungeon game
    protocol flows.
  - A C++ Discord Activity backend at `app/activity-server` that uses the
    runtime directly for `/healthz`, queue/download/play APIs, HLS serving,
    HLS proxying, thumbnail proxying, and WebSocket room state updates.
- **Homelab operations**
  - Dockerized services for the Activity backend and deployable example apps.
  - GitHub Actions workflows that build GHCR images for `dropapp` and
    `webhook-inbox`, then update the home GitOps repository with SHA-pinned
    image tags.
  - Kubernetes home manifests for k3s, ingress-nginx, MetalLB, Argo CD, and a
    demo workload under `deploy/k8s/home`.
  - Production-oriented service assets for systemd, env files, metrics files,
    local persistent storage, and reverse-proxy integration.

Current implementation status:

- Core runtime, observability, media utilities, and Activity C++ server build
  by default.
- Optional `RuntimeWeb`, `RuntimeProxy`, and `RuntimeGame` modules are enabled
  with `-DBUILD_WEB=ON`, `-DBUILD_PROXY=ON`, and `-DBUILD_GAME=ON`.
- `app/activity-backend` remains as the FastAPI migration source; TVING cookie
  playback, Netflix proxying, and WebRTC proxying still need to be ported
  before it can be removed.

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

An optional multiplayer game packet module can also be built from the same
source tree:

- `iouring_runtime_game::RuntimeGame`
- public headers under `iouring_runtime/game/...`
- enabled only with `-DBUILD_GAME=ON`
- includes packet framing, player/session registry, room dispatch, and room
  management helpers
- includes a protobuf packet echo example when `protoc` is available

A reusable media utility module is installed with the core package:

- `iouring_runtime::RuntimeMedia`
- public headers under `iouring_runtime/media/...`
- currently includes HLS manifest rewriting, URL decoding, and HLS content type
  helpers used by the Activity server

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

## Repository Layout

- `include/iouring_runtime/core/`
  public runtime headers for rings, listeners, sessions, buffers, queues,
  timers, and core types
- `include/iouring_runtime/web/`
  public headers for the optional HTTP module
- `include/iouring_runtime/proxy/`
  public headers for the optional TCP proxy module
- `include/iouring_runtime/game/`
  public headers for the optional multiplayer packet module
- `include/iouring_runtime/media/`
  public headers for reusable media/HLS helpers
- `include/iouring_runtime/observability/`
  public logging/profiling helpers shared by modules
- `src/runtime/`
  core runtime implementation split by `ring`, `io`, `job`, and `common`
- `src/modules/web/`
  HTTP parser/session/router/server implementation
- `src/modules/proxy/`
  TCP reverse proxy implementation, including TLS and ACME helper sessions
- `src/modules/game/`
  packet framing, player/session registry, room dispatch, and room-management
  helpers for binary multiplayer protocols
- `src/modules/media/`
  media helper implementation shared by apps and tests
- `src/modules/observability/`
  logging implementation
- `app/activity-server/`
  C++ io_uring Discord Activity backend
- `app/activity-backend/`
  FastAPI Activity backend kept as the migration source until feature parity
- `app/examples/runtime/`, `app/examples/web/`, `app/examples/proxy/`, `app/examples/game/`
  runnable examples for each layer
- `tests/`
  focused tests grouped by runtime, modules, and protocol behavior
- `scripts/`
  sanitizer, wrk, benchmark, and comparison helpers
- `deploy/`
  Kubernetes manifests used by example deployments

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

Optional game packet module:

```bash
cmake -S . -B build-game -DCMAKE_BUILD_TYPE=Release -DBUILD_GAME=ON
cmake --build build-game -j$(nproc)
cmake --install build-game --prefix /tmp/iouring-runtime-install
```

## Examples

- `app/examples/runtime/core_echo/`
- `app/examples/runtime/core_idle_echo/`
- `app/examples/web/dropapp/`
- `app/examples/proxy/tcp_reverse_proxy/`
  deployment assets: `app/examples/proxy/tcp_reverse_proxy/deploy/`
- `app/examples/game/dungeon_packet_echo/`
  protobuf packet example for login, room list, room creation, and room join
  flow over the dungeon RPG wire protocol
- `app/examples/game/dungeon_full_server/`
  full dungeon RPG gameplay server port layered on `RuntimeGame`, with SQLite
  or in-memory account, character, inventory, and currency storage

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
  DROPAPP_STATIC_ROOT=app/examples/web/dropapp/static \
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
  WEBHOOK_INBOX_STATIC_ROOT=app/examples/web/webhook_inbox/static \
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
