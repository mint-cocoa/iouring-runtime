# runtime Architecture

This is a maintainer note for the current module layout. For user-facing
examples, start with `docs/getting-started.md` and `docs/usage-examples.md`.

## Module Boundaries

```text
CMake target: module-level link boundary
C++ include:  per-file public API selection
```

| Module | Target | Source | Public headers |
| --- | --- | --- | --- |
| Core runtime | `iouring::runtime` | `src/core/` | `include/iouring/core/` |
| Observability | `iouring::observability` | `src/observability/` | `include/iouring/observability/` |
| Media | `iouring::media` | `src/media/` | `include/iouring/media/` |
| Web | `iouring::http` | `src/http/` | `include/iouring/http/` |
| Proxy | `iouring::stream` | `src/stream/` | `include/iouring/stream/` |
| Game | `iouring::game` | `src/game/` | `include/iouring/game/` |

The core target is protocol-agnostic. Web, proxy, and game code build on top of
core without moving protocol concepts into `include/iouring/core/`.

## Core Responsibilities

Core owns:

- `io_uring` creation and dispatch
- listener accept flow
- session lifecycle and operation-owned drain
- recv registration
- send queue draining and short-write retry
- send/recv buffers
- job queues and timers

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
Worker::Start
IoRing::Create
BufferPool
SessionFactory
Listener::Start
ring.Dispatch loop
Session::OnRecv
Session::Send
Session::Disconnect
```

`Worker` is the standard execution unit: one thread, one `IoRing`, one
`BufferPool`, one `Listener`, and the sessions accepted on that listener.
`Listener` accepts sockets and asks a `SessionFactory` to create a protocol
session. The runtime owns the I/O lifecycle; the session subclass owns protocol
behavior.

Cross-thread work is sent to a ring with `IoRing::Post` / `RunOnRing`.
Posted work wakes the target ring, so owner-thread operations such as session
disconnect, listener stop, and cross-worker sends do not wait for the dispatch
timeout.

## Optional Modules

`iouring::http` adds:

- HTTP parsing
- route matching
- middleware
- request params, query params, cookies
- response helpers
- file and streaming responses
- worker-thread server wrapper

`iouring::stream` adds:

- downstream TCP listener
- upstream connector
- bidirectional stream bridge
- optional downstream TLS termination
- SNI-based upstream routes
- standalone ACME HTTP-01 challenge server
- metrics snapshots

`iouring::game` adds:

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
./tools/run_runtime_sanitizers.sh
```
