# Web Benchmarking

This repository includes an optional HTTP example and a ready-to-run `wrk`
script for quick local load checks.

## Tool

- `wrk`: https://github.com/wg/wrk

The script expects `wrk` and `curl` to be available in `PATH`.

If you want to reproduce the same runs on another workstation, server, or
bare-metal Linux machine, see:

- `docs/benchmarking-on-other-machines.md`

## Quick Start

Run the integrated runtime suite when you want Core echo, Web, Proxy, Game
fan-out, and benchmark-only `epoll` references in one result directory:

```bash
SUITE_MODE=quick ./benchmarks/run_runtime_bench_suite.sh
```

Standard mode repeats benchmark scenarios three times and writes median-ready
artifacts under `benchmark-results/runtime-suite/<timestamp>/`:

```bash
./benchmarks/run_runtime_bench_suite.sh
```

Run the default medium preset against `hello_http` from
`iouring-runtime-examples`:

```bash
./benchmarks/run_hello_http_wrk.sh
```

Available presets:

- `light`
- `medium`
- `heavy`
- `soak`

Example:

```bash
./benchmarks/run_hello_http_wrk.sh heavy
```

Run the multi-scenario suite and save raw results plus analysis:

```bash
./benchmarks/run_hello_http_wrk_suite.sh
```

For a faster local pass:

```bash
SUITE_MODE=quick ./benchmarks/run_hello_http_wrk_suite.sh
```

Run a configuration matrix across multiple `WebServerConfig` combinations:

```bash
./benchmarks/run_hello_http_wrk_matrix.sh
```

For a shorter pass:

```bash
MATRIX_MODE=quick ./benchmarks/run_hello_http_wrk_matrix.sh
```

Compare the same `wrk` scenarios against `iouring`, `nginx`, and
`caddy`:

```bash
./benchmarks/run_reference_webserver_wrk_compare.sh
```

For a shorter pass:

```bash
MODE=quick ./benchmarks/run_reference_webserver_wrk_compare.sh
```

## What The Script Does

The script:

1. configures a Release build with `BUILD_HTTP=ON`
2. builds `hello_http`
3. starts the server on `127.0.0.1:$PORT`
4. waits for `/health`
5. runs `wrk` with the selected preset
6. shuts the server down on exit

Server logs are written to:

- `build-web-bench/hello_http.log`
- suite output goes under `benchmark-results/wrk/<timestamp>/`

## Environment Overrides

You can override the defaults without editing the script:

```bash
PORT=18080 THREADS=6 CONNECTIONS=192 DURATION=20s ./benchmarks/run_hello_http_wrk.sh medium
```

Supported variables:

- `BUILD_DIR`
- `TARGET`
- `HOST`
- `PORT`
- `PATH_SUFFIX`
- `HELLO_HTTP_WORKERS`
- `HELLO_HTTP_LOG_LEVEL`
- `THREADS`
- `CONNECTIONS`
- `DURATION`
- `RESULTS_BASE_DIR`
- `SUITE_MODE`
- `MATRIX_MODE`
- `MODE`
- `WORKER_SWEEP_MODE`
- `MAX_AUTO_WORKERS`
- `AUTO_TUNED_WORKERS`
- `CPU_SAMPLE_INTERVAL`
- `SERVER_FILTER`

## Suggested Usage

Use these as rough starting points:

- `light`: quick smoke check after a change
- `medium`: normal local regression comparison
- `heavy`: short saturation check
- `soak`: longer stability run for hangs, leaks, or shutdown issues

The suite currently covers:

- smoke GET `/`
- steady GET `/`
- mixed GET traffic across `/` and `/health`
- HEAD `/health`
- higher-connection saturation on `/`
- longer soak on `/`

The config matrix currently explores:

- `worker_count`
- `ring.queue_depth`
- `ring.buf_count`
- `ring.submit_batch_size`
- `ring.cqe_batch_budget`
- `ring.io_timeout`
- per-workload CPU sampling for the server process and overall system

By default the matrix uses `WORKER_SWEEP_MODE=auto`, which builds baseline
worker-count cases from the current machine's logical CPU count such as `1, 2,
4, 8, ...` up to the capped maximum, then adds tuned variants around
`AUTO_TUNED_WORKERS`.

## Reading The Results

Pay attention to:

- `Requests/sec`
- latency percentiles from `--latency`
- socket errors or timeouts
- whether throughput or latency changes sharply across presets

For this repository, `wrk` is best used as a first-pass HTTP stability and
throughput check. Pair it with the existing tests and sanitizer runs for a
stronger confidence read.

## runtime Tuning

`hello_http` currently exposes a few useful runtime knobs via environment
variables:

- `HELLO_HTTP_WORKERS`: number of web worker threads
- `HELLO_HTTP_WORKER_AFFINITY`: `off`, `physical`, or `logical`
- `HELLO_HTTP_LOG_LEVEL`: spdlog level such as `trace`, `debug`, `info`, `warn`, `error`, `critical`, or `off`

Examples:

```bash
HELLO_HTTP_WORKERS=8 HELLO_HTTP_LOG_LEVEL=warn ./benchmarks/run_hello_http_wrk.sh medium
```

```bash
HELLO_HTTP_WORKERS=16 HELLO_HTTP_WORKER_AFFINITY=physical HELLO_HTTP_LOG_LEVEL=off ./benchmarks/run_hello_http_wrk.sh medium
```

```bash
HELLO_HTTP_WORKERS=$(nproc) HELLO_HTTP_LOG_LEVEL=off MODE=quick ./benchmarks/run_reference_webserver_wrk_compare.sh
```
