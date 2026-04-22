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

The runtime does not own:

- HTTP
- WebSocket
- packet schemas
- storage
- application-domain logic

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
