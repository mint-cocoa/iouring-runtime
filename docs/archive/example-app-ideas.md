# Archived Example App Ideas

This file is kept as historical planning context. Active usage documentation
lives in `README.md`, `docs/getting-started.md`, and
`docs/usage-examples.md`.

`llm.txt` describes the deployment contract for an app that will run on the
home k3s cluster:

- container image: `ghcr.io/mint-cocoa/<appname>:${GITHUB_SHA}`
- public host: `<appname>.k8s.mintcocoa.cc`
- ingress class: `nginx`
- service port: `80`
- container port: `3000`
- deployment path: update `mint-cocoa/home-k8s-gitops` and let Argo CD sync
- required app endpoint: `/healthz`

This repository currently exposes two useful layers for example apps:

- `iouring::core`: protocol-agnostic TCP runtime with listeners,
  sessions, send/recv buffers, timers, watchdogs, and backpressure.
- `iouring::http`: optional HTTP module with routing, middleware,
  request params, query/cookie helpers, response helpers, and `SendFile`.

The best example app should show why this runtime exists without turning the
runtime repository into a general application framework.

## Recommended App: Realtime Probe Board

Build a small HTTP service named `probe-board`.

The app exposes a dashboard and JSON endpoints that report process/runtime
state, synthetic latency probes, and request counters. It is intentionally
state-light, easy to containerize, and useful in the home lab as a living
canary for the `io_uring` stack.

### Why This Fits

- Uses the current `iouring_http` stack directly.
- Keeps persistence out of scope, so no database or secret handling is needed.
- Exercises routing, middleware, timers, JSON responses, static assets, and
  health checks.
- Works naturally at `/` behind ingress without subpath assumptions.
- Provides a realistic target for wrk benchmarks and GitOps deployment.

### User-Facing Shape

- `GET /`: small HTML dashboard.
- `GET /healthz`: returns `ok` for Kubernetes probes.
- `GET /api/status`: runtime config, uptime, worker count, build SHA.
- `GET /api/metrics`: request counters, status counters, bytes sent estimate,
  per-route counters.
- `GET /api/probe/latency`: schedules a tiny timer/job and returns observed
  scheduling latency in microseconds.
- `GET /payload/:size`: optional benchmark payload endpoint copied from the
  existing `hello_http` example pattern.

The dashboard can poll `/api/status` and `/api/metrics` every second. It should
stay simple: one static HTML file, one CSS file, and no frontend build step.

### Implementation Plan

- Add `examples/http/probe_board/`.
- Build it only when `-DBUILD_HTTP=ON -DBUILD_EXAMPLES=ON`.
- Reuse the existing `WebServer` setup pattern from `examples/http/hello_http`.
- Read config from environment variables:
  - `PROBE_BOARD_PORT`, default `3000`
  - `PROBE_BOARD_WORKERS`, default `1`
  - `PROBE_BOARD_LOG_LEVEL`, default inherited from spdlog
  - `PROBE_BOARD_BUILD_SHA`, default `dev`
- Add middleware that increments in-memory counters before dispatch.
- Use `std::atomic` counters for metrics so the app remains dependency-free.
- Serve dashboard assets either as embedded strings or from a small static
  directory. Embedded strings make the container simpler.
- Return JSON manually at first; if the app grows, introduce a tiny local JSON
  helper in the example only.

### Docker

Use a multi-stage image:

- builder: Debian/Ubuntu base with `cmake`, `ninja-build`, `g++`, `pkg-config`,
  and `liburing-dev`
- runtime: slim Debian/Ubuntu with `liburing`
- configure with `-DBUILD_HTTP=ON -DBUILD_EXAMPLES=ON -DBUILD_TESTS=OFF`
- copy only `probe_board` into the runtime image
- expose container port `3000`
- run as a non-root user if the base image setup stays simple

### GitHub Actions

Workflow responsibilities:

- build the Docker image on push to the app branch or main
- tag as `ghcr.io/mint-cocoa/probe-board:${GITHUB_SHA}`
- push to GHCR
- check out `mint-cocoa/home-k8s-gitops`
- update `apps/probe-board/values.yaml` image tag to `${GITHUB_SHA}`
- commit and push the GitOps change

The workflow needs a token that can push to the GitOps repo. Do not commit that
token or any secret value.

### GitOps Shape

Create these files in `mint-cocoa/home-k8s-gitops`:

- `clusters/home/probe-board-application.yaml`
- `app/probe-board/Chart.yaml`
- `apps/probe-board/values.yaml`
- `app/probe-board/templates/deployment.yaml`
- `app/probe-board/templates/service.yaml`
- `app/probe-board/templates/ingress.yaml`

Suggested values:

```yaml
image:
  repository: ghcr.io/mint-cocoa/probe-board
  tag: replace-by-github-sha

service:
  port: 80
  targetPort: 3000

ingress:
  className: nginx
  host: probe-board.k8s.mintcocoa.cc
  path: /
```

Kubernetes probes should use:

```yaml
readinessProbe:
  httpGet:
    path: /healthz
    port: 3000
livenessProbe:
  httpGet:
    path: /healthz
    port: 3000
```

## Alternative 1: Minimal Todo API

Build `todo-app` as a tiny JSON API over `iouring_http`.

Endpoints:

- `GET /`
- `GET /healthz`
- `GET /api/todos`
- `POST /api/todos`
- `PUT /api/todos/:id`
- `DELETE /api/todos/:id`

This matches the GitOps path already mentioned in `llm.txt`
(`apps/todo-app/values.yaml`). The downside is that durable storage becomes the
main question. For a first example, either keep todos in memory and document
that restarts clear state, or add file-backed JSON later outside the runtime
core.

## Alternative 2: Core TCP Latency Echo

Build `tcp-latency-echo` directly on `iouring::core`.

Protocol:

- client sends newline-delimited frames
- server echoes the frame with a timestamp prefix
- idle sessions close through the existing watchdog pattern

This is the cleanest demonstration of the core runtime. It is less convenient
for the current ingress setup because normal Kubernetes ingress-nginx HTTP
rules do not expose arbitrary TCP services without extra controller config.
Choose this when the goal is runtime validation rather than browser-visible app
deployment.

## Alternative 3: Static Payload Lab

Build `payload-lab`, a benchmark-oriented HTTP app.

Endpoints:

- `GET /healthz`
- `GET /payload/256b`
- `GET /payload/4k`
- `GET /payload/64k`
- `GET /payload/1m`
- `GET /api/config`

This is closest to the existing benchmark scripts and `hello_http` example. It
is excellent for wrk comparisons, but less interesting as a standalone app
because it mostly serves fixed bytes.

## Pick

Start with `probe-board`.

It gives the runtime repository a useful deployed example without introducing a
database, auth, or application domain. It also creates a stable home-lab canary:
if `probe-board.k8s.mintcocoa.cc/healthz` and the dashboard are healthy, the
image build, ingress path, GitOps sync, and `iouring_http` stack are all working.
