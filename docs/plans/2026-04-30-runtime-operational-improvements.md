# Runtime Operational Improvements

## Goal

Improve real-service behavior before chasing benchmark-only score changes.

The priority is to make the runtime easier to operate under common production
conditions:

- slow handlers
- slow clients
- large responses
- client disconnects and resets
- bounded memory use under pressure
- low-noise logs and useful counters

## Current Status

The latest runtime improvement commit is:

```text
523d5cc Improve runtime backpressure and request observability
```

That commit added:

- byte-based send queue backpressure
- send queue pending byte and peak byte accounting
- slow request logging for HTTP sessions
- lower fast-path request observability cost when access logging and slow
  request logging are disabled
- thread-local HTTP Date header caching
- send queue drain reuse to avoid repeated vector allocation in the hot path

Focused tests passed after that change:

- `./build/bin/test_session_partial_send`
- `./build-web/bin/test_http_response_framing`
- `./build-web/bin/test_http_parser`
- `./build-web/bin/test_router`

Tests that create a live `io_uring` can fail inside the Codex sandbox because
the sandbox blocks the required syscalls. Running the same native WSL probe
outside that sandbox created the ring successfully.

## Operational Probe Results

A temporary native WSL probe server was used to validate the new behavior
outside the sandbox.

Configuration:

- one HTTP worker
- slow request threshold: `10ms`
- send queue high byte watermark: `64 KiB`
- send queue low byte watermark: `16 KiB`
- disconnect on high watermark: enabled

Observed behavior:

- `/health` returned `200` normally.
- `/slow` delayed for about `30ms` and emitted a `[SLOW_REQUEST]` warning.
- `/large` attempted a multi-megabyte response and triggered
  `[DISC:SLOW_CLIENT] send queue pressure ... bytes=3145888`, then the
  connection was closed as configured.

Short local `wrk` checks against `/health` also completed successfully:

```text
wrk -t2 -c32 -d5s /health
Requests/sec: 83267.35
Avg latency: 380.63us
Socket errors: none
```

## Findings

The implemented features are working at the behavior level:

- slow request detection fires at the configured threshold
- byte-based backpressure detects oversized pending send queues
- high-watermark disconnect protects memory for slow-client scenarios

The probe also exposed two follow-up issues:

1. Client resets such as `ECONNRESET` are logged as errors.
   This creates noisy logs during normal load-test shutdowns and common
   real-world disconnects.

2. `HttpResponse::Text`, `Json`, and `Body` still build the full response into
   one send buffer. Responses larger than the current buffer pool chunk can fail
   to build instead of being sent in chunks or via the streaming path.

## Next Improvement Plan

### 1. Normalize disconnect logging

Treat common client-side connection shutdowns as expected transport outcomes:

- peer close
- `ECONNRESET`
- `EPIPE`
- send after client close during shutdown or benchmark teardown

Expected result:

- normal disconnects do not flood error logs
- true runtime send/recv failures remain visible
- disconnect reasons are easier to classify

Proof:

- focused unit coverage for errno classification
- short `wrk` run should not produce error-level reset noise

### 2. Improve large HTTP response handling

Avoid requiring one contiguous send buffer for every non-file response.

Candidate approach:

- keep current fast path for small responses
- split large body responses into header plus body chunks
- preserve `Content-Length` when possible
- route very large bodies through existing stream/file-style send flow

Expected result:

- large JSON/text/HTML responses succeed predictably
- memory pressure is bounded
- backpressure can act on queued chunks instead of a single oversized buffer

Proof:

- unit coverage for responses above one buffer chunk
- native WSL probe for multi-megabyte body success and slow-client cutoff

### 3. Refine backpressure policy

The current policy can disconnect immediately on high watermark. That is useful
for defense but too blunt as the only operational mode.

Add policy options for:

- pause reads while outbound pressure is active
- disconnect only after pressure stays active past a timeout
- endpoint or session class specific limits

Expected result:

- good clients survive short bursts
- persistently slow clients are still bounded
- proxy and HTTP users can choose different pressure behavior

Proof:

- transition tests for active/inactive backpressure
- slow-reader integration probe

### 4. Add runtime counters

Expose lightweight counters before integrating with any external metrics stack.

Initial counters:

- active sessions
- requests by status class
- slow request count
- disconnect reason counts
- send queue high-watermark count
- send queue peak depth and peak bytes
- parser error count

Expected result:

- easier production debugging
- benchmark runs have supporting runtime evidence beyond `wrk` output

Proof:

- snapshot API tests
- probe output under controlled slow request and slow client scenarios

## Execution Order

Start with disconnect logging because it is small, low-risk, and improves the
signal quality for every later test.

Then fix large response handling, because it affects real user responses and
was directly exposed during the operational probe.

Backpressure policy refinement and counters should follow after those two
changes, because they benefit from cleaner disconnect classification and better
large-response behavior.
