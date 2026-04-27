# Runtime Guide

This guide is for consumers who want to start from the runtime target directly
and use the repository as an `io_uring` runtime rather than as a higher-level
server framework.

## What Runtime Means Here

In this repository, `Runtime` means the lower-level execution substrate:

- `io_uring` creation and dispatch
- listener accept flow
- session lifetime and self-ownership
- send/recv buffering
- timeout and watchdog behavior
- backpressure and drain semantics

Today that layer is exposed as:

- `iouring_runtime::Runtime`

and the public headers under:

- `iouring_runtime/core/...`

## When To Start With The Runtime

Start with `iouring-runtime` when you are building:

- a custom binary protocol server
- a small TCP service that does not need HTTP routing
- a transport/runtime experiment around `io_uring`
- a lower-level service where lifecycle control matters more than web helpers

If you need HTTP, use the optional `RuntimeWeb` module. WebSocket, storage,
packet schemas, and application-specific logic should live in layers above the
runtime core.

## Public Runtime Surface

The core public types are:

- `iouring_runtime::core::ring::IoRing`
- `iouring_runtime::core::io::Listener`
- `iouring_runtime::core::io::Session`
- `iouring_runtime::core::buffer::SendBuffer`
- `iouring_runtime::core::buffer::RecvBuffer`
- `iouring_runtime::core::buffer::SendQueue`

This surface is intentionally small. It is meant to be a set of reusable
building blocks rather than a high-level framework.

## Runtime Examples

Current runtime-facing examples:

- `app/examples/runtime/core_echo/`
  minimal runtime echo server
- `app/examples/runtime/core_idle_echo/`
  echo server with inactivity watchdog and graceful close-after-flush behavior

Build them with:

```bash
cmake -S . -B build-runtime \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-runtime --target core_echo core_idle_echo -j$(nproc)
```

Run them with:

```bash
CORE_ECHO_PORT=19090 ./build-runtime/bin/core_echo
CORE_IDLE_ECHO_PORT=19091 CORE_IDLE_TIMEOUT_MS=5000 ./build-runtime/bin/core_idle_echo
```

## Minimal Runtime Shape

The runtime usage pattern is:

1. create an `IoRing`
2. create a `BufferPool`
3. define a `Session` subclass
4. provide a `SessionFactory`
5. construct a `Listener`
6. drive `Dispatch()` in a loop

That shape is visible in both runtime examples and is the key distinction
between this repository and a higher-level server framework.

## Runtime Design Rules

The runtime should stay:

- protocol-agnostic
- lifecycle-focused
- small in public API surface
- strong on shutdown and backpressure behavior

That means `Runtime` should not absorb:

- HTTP request/response types
- routers or middleware
- WebSocket upgrade logic
- static file policy
- auth helpers
- application-domain concepts

The repository may still contain optional modules that use the runtime. Those
modules should remain separate targets with their public headers outside
`iouring_runtime/core/...`.

## Related Docs

- Getting started: `docs/getting-started.md`
- API reference: `docs/api-reference.md`
- Runtime architecture: `docs/runtime-architecture.md`
- Runtime plan: `docs/plans/2026-04-22-io-uring-runtime-plan.md`
