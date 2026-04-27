# io_uring Runtime Plan

## Goal

Turn the repository into a clearly legible standalone `io_uring Runtime`
product:

- protocol-agnostic
- reusable without `ServerWeb`
- strong on lifecycle correctness and shutdown semantics
- documented and demonstrated as an independent runtime layer

This plan assumes the repository itself is runtime-only. Higher protocol or
application layers should live above it, not beside it in the same package.

## Product statement

Target shape for downstream consumers:

```text
App
  -> Lib
     -> Runtime(iouring-runtime)
```

## Runtime definition

`Runtime` is the execution substrate. It should own:

- `io_uring` setup and dispatch
- listener accept flow
- session lifetime and ownership
- recv/send queueing
- buffer pooling
- timeout and watchdog behavior
- backpressure signaling
- drain and shutdown semantics

It should not own:

- HTTP parsing or routing
- WebSocket framing or upgrade policy
- static file policy
- reverse proxy policy
- JWT/cookie auth
- application-domain concepts

## Current baseline

The repository already has a strong starting point:

- `src/io/*`
- `src/ring/*`
- `src/job/*`
- `tests/io/*`
- `app/examples/runtime/core_echo/`

The main remaining gap is not “missing runtime code” so much as “runtime
identity is still easier to miss than the web framework layer”.

## Plan phases

### Phase 0: runtime identity

Make `iouring-runtime` visibly consumable as a standalone runtime package.

Deliverables:

- runtime-facing guide
- runtime example grouping in docs
- runtime-only examples that show lifecycle behavior

### Phase 1: boundary freeze

Keep `Runtime` protocol-agnostic.

Rules:

- no HTTP/WS/storage/app types in `servercore`
- protocol and application bridge types stay outside runtime
- new runtime APIs must be reusable by non-web protocols

### Phase 2: lifecycle hardening

Strengthen the execution contract around:

- pending I/O ownership
- partial send behavior
- inactivity watchdogs
- drain and force-close shutdown paths
- slow-client backpressure transitions

Primary proof points:

- focused tests
- sanitizer runs
- stress scripts

### Phase 3: runtime ergonomics

Make the runtime easier to compose directly:

- clearer guide for custom protocol servers
- more than one runtime-only example
- stronger documentation around session and listener responsibilities

### Phase 4: runtime observability

Add lightweight runtime-facing visibility into:

- live session counts
- send queue depth
- timeout-triggered disconnects
- forced shutdown closes
- worker drain progress

This can start as internal stats snapshots before any external metrics system.

## Immediate execution slice

This repository baseline implements the first small slice of the plan:

1. add a dedicated runtime guide
2. add a second runtime example focused on inactivity watchdog behavior
3. wire runtime docs/examples more clearly into the public entry docs

This keeps the change small while making the runtime identity noticeably
clearer for new readers and consumers.

## Success criteria

This plan is working when a new reader can quickly infer:

1. the repository is recognizably a reusable `io_uring` runtime product.
2. downstream protocol or server libraries can build on it without changing
   the runtime surface.
3. a custom protocol server can start from the runtime examples without first
   learning any web or game framework surface.
