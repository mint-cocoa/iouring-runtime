# Runtime Architecture

This document describes the current architecture of the standalone
`iouring-runtime` repository.

## Scope

The runtime owns:

- `io_uring` creation and dispatch
- listener accept flow
- session lifecycle and self-ownership
- send/recv buffering
- timeout and watchdog handling
- queueing and backpressure
- job queues and timers

The core runtime target does not own:

- HTTP
- WebSocket
- packet schemas
- storage
- application-domain logic

HTTP, TCP proxy, multiplayer packet, and observability code now live as
optional modules in the same repository. They build on the runtime core
without changing the default runtime package boundary.

## Source Layout

- `include/iouring_runtime/core/`
  public runtime API
- `include/iouring_runtime/web/`
  optional HTTP API
- `include/iouring_runtime/proxy/`
  optional TCP proxy API
- `include/iouring_runtime/game/`
  optional multiplayer packet, player registry, and room API
- `include/iouring_runtime/observability/`
  logging and profiling helpers
- `src/runtime/ring/`
  `io_uring` wrapper and buffer-ring support
- `src/runtime/io/`
  listener and session lifecycle implementation
- `src/runtime/job/`
  job queues and timers
- `src/runtime/common/`
  shared runtime utilities such as CPU affinity helpers
- `src/modules/web/`
  HTTP parser, response framing, router, sessions, streaming body handling,
  deferred responses, file streaming, and worker server logic
- `src/modules/proxy/`
  TCP proxy server, upstream connector, bridge logic, TLS context, and ACME
  challenge session support
- `src/modules/game/`
  packet framing, player/session registry, room dispatch, and room-management
  helpers for binary multiplayer protocols
- `src/modules/observability/`
  logging implementation used by runtime and modules

## Core Types

The execution model centers on:

- `iouring_runtime::core::ring::IoRing`
- `iouring_runtime::core::io::Listener`
- `iouring_runtime::core::io::Session`
- `iouring_runtime::core::buffer::SendBuffer`
- `iouring_runtime::core::buffer::RecvBuffer`
- `iouring_runtime::core::buffer::SendQueue`
- `iouring_runtime::core::job::GlobalQueue`
- `iouring_runtime::core::job::JobQueue`
- `iouring_runtime::core::job::JobTimer`

## Accept Path

- listeners are created with nonblocking sockets
- accepted sockets are normalized before session construction
- multishot accept is the primary path
- `Listener::OnAccept()` constructs sessions through a `SessionFactory`

This keeps protocol creation above the runtime while letting the runtime own
the actual accept loop.

## Session Model

`Session` is the key runtime abstraction.

It owns:

- recv registration
- send queue drain and short-write retry
- disconnect and disconnect-after-flush behavior
- inactivity watchdog timing
- self-ownership while I/O is in flight

Subclasses provide only protocol-level behavior through:

- `OnRecv(...)`
- `OnConnected()`
- `OnDisconnected()`
- `OnTimeoutTick(...)`
- `HasPendingAppWork()`

## Backpressure

The runtime currently supports:

- send-queue hard overflow limits
- optional high/low watermark transitions
- optional disconnect-on-high-watermark policy

These policies are enforced in the runtime, not in any protocol layer.

## Shutdown

At the session level, shutdown semantics are:

- `Disconnect()` for immediate disconnect initiation
- `DisconnectAfterFlush()` for close-after-queued-send behavior
- self-ownership release only after in-flight I/O settles

Repository-level process shutdown is left to the consuming application or
example entrypoint.

## Validation

The runtime is currently validated through:

- focused unit tests under `tests/`
- example programs under `examples/runtime/`
- sanitizer rebuilds via `scripts/run_runtime_sanitizers.sh`

Optional module behavior is covered by:

- HTTP tests under `tests/web/`
- proxy tests under `tests/proxy/`
- observability tests under `tests/observability/`
- web, proxy, and game examples under `examples/web/`, `examples/proxy/`,
  and `examples/game/`; the full dungeon RPG gameplay port lives under
  `examples/game/dungeon_full_server/`
