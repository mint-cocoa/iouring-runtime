# Runtime Architecture

This is a maintainer note for the current module layout. For user-facing
examples, start with `docs/getting-started.md` and `docs/usage-examples.md`.

## Module Boundaries

```text
CMake target: module-level link boundary
C++ include:  per-file public API selection
```

| Module | Target | Source | Public headers |
| --- | --- | --- | --- |
| Core runtime | `iouring_runtime::Runtime` | `src/runtime/` | `include/iouring_runtime/core/` |
| Observability | `iouring_runtime::RuntimeObservability` | `src/modules/observability/` | `include/iouring_runtime/observability/` |
| Media | `iouring_runtime::RuntimeMedia` | `src/modules/media/` | `include/iouring_runtime/media/` |
| Web | `iouring_runtime_web::RuntimeWeb` | `src/modules/web/` | `include/iouring_runtime/web/` |
| Proxy | `iouring_runtime_proxy::RuntimeProxy` | `src/modules/proxy/` | `include/iouring_runtime/proxy/` |
| Game | `iouring_runtime_game::RuntimeGame` | `src/modules/game/` | `include/iouring_runtime/game/` |

The core target is protocol-agnostic. Web, proxy, and game code build on top of
core without moving protocol concepts into `include/iouring_runtime/core/`.

## Core Responsibilities

Core owns:

- `io_uring` creation and dispatch
- listener accept flow
- session lifecycle and self-ownership
- recv registration
- send queue draining and short-write retry
- send/recv buffers
- inactivity timeout hooks
- job queues and timers
- backpressure policy

Core does not own:

- HTTP parsing or routing
- WebSocket upgrade policy
- packet schemas
- TLS termination policy
- storage drivers
- application domain objects

## Accept And Session Flow

The usual core flow is:

```text
IoRing::Create
BufferPool
SessionFactory
Listener::Start
ring.Dispatch loop
Session::OnRecv
Session::Send
Session::Disconnect or DisconnectAfterFlush
```

`Listener` accepts sockets and asks a `SessionFactory` to create a protocol
session. The runtime owns the I/O lifecycle; the session subclass owns protocol
behavior.

## Optional Modules

`RuntimeWeb` adds:

- HTTP parsing
- route matching
- middleware
- request params, query params, cookies
- response helpers
- file and streaming responses
- worker-thread server wrapper

`RuntimeProxy` adds:

- downstream TCP listener
- upstream connector
- bidirectional stream bridge
- optional downstream TLS termination
- optional ACME HTTP-01 responder
- SNI-based upstream routes
- metrics snapshots

`RuntimeGame` adds:

- packet framing
- packet builder helpers
- packet sessions
- player registry
- room and room manager helpers

## Validation

Tests are grouped by runtime area:

- `tests/core/`
- `tests/io/`
- `tests/ring/`
- `tests/job/`
- `tests/web/`
- `tests/proxy/`
- `tests/game/`
- `tests/media/`
- `tests/observability/`

Run the full configured suite:

```bash
ctest --test-dir build-tests --output-on-failure
```

Run sanitizer builds:

```bash
./scripts/run_runtime_sanitizers.sh
```
