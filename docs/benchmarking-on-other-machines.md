# Running Benchmarks On Another Machine

This guide is for reproducing the web benchmark runs on a different computer so
that results from WSL2, bare-metal Linux, or another VM can be compared using
the same scripts and result layout.

## Why Run Elsewhere

The current benchmark work was developed on WSL2. That is useful for fast local
iteration, but it can change scheduler behavior, CPU migration patterns, and
loopback networking costs. Running the same suite on another machine helps us
separate:

- runtime behavior
- Linux scheduler effects
- WSL2 or virtualization effects
- machine-specific CPU topology effects

## Recommended Targets

Use one of these targets:

- bare-metal Linux on the same class of CPU as the main development machine
- a second Linux workstation or server
- a clean VM if bare metal is not available

If possible, prefer bare-metal Linux for the final performance read.

## Prerequisites

Install:

- a C++23 compiler such as `g++` or `clang++`
- `cmake`
- `python3`
- `curl`
- `wrk`
- standard build tools such as `make` or `ninja`

Optional:

- `docker`
  for `scripts/run_reference_webserver_wrk_compare.sh`
- `pidstat` and `mpstat`
  for richer CPU-side analysis

Example on Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build python3 curl wrk sysstat docker.io
```

## Clone The Exact Revision

Use the same commit on every machine.

```bash
git clone <your-repo-url>
cd iouring-runtime
git checkout <commit-sha>
git rev-parse HEAD
```

If this repository is being copied manually instead of cloned, make sure the
same source tree and commit metadata are preserved in your test notes.

## Capture Machine Metadata First

Before running any benchmark, save the machine profile next to the benchmark
artifacts.

```bash
mkdir -p benchmark-results/host-info
{
  date -Is
  echo
  uname -a
  echo
  lscpu
  echo
  cat /proc/version
  echo
  cat /proc/sys/kernel/osrelease
} > benchmark-results/host-info/$(hostname)-env.txt
```

For WSL2, also record:

```bash
grep -i microsoft /proc/version /proc/sys/kernel/osrelease || true
```

## Build Once

Use a clean release build for benchmark runs.

```bash
cmake -S . -B build-web-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF

cmake --build build-web-bench --target hello_http -j"$(nproc)"
```

## Recommended Benchmark Order

Run the scripts in this order.

### 1. Quick Sanity Check

```bash
HELLO_HTTP_LOG_LEVEL=off ./scripts/run_hello_http_wrk.sh medium
```

### 2. Multi-Scenario Suite

```bash
HELLO_HTTP_LOG_LEVEL=off SUITE_MODE=quick ./scripts/run_hello_http_wrk_suite.sh
```

### 3. Worker And Ring Matrix

```bash
HELLO_HTTP_LOG_LEVEL=off MATRIX_MODE=quick ./scripts/run_hello_http_wrk_matrix.sh
```

### 4. Worker vs `wrk -t` Thread Matrix

```bash
HELLO_HTTP_LOG_LEVEL=off MATRIX_MODE=quick ./scripts/run_hello_http_wrk_thread_matrix.sh
```

### 5. Reference Server Comparison

Run this only if Docker is available.

```bash
HELLO_HTTP_LOG_LEVEL=off MODE=quick ./scripts/run_reference_webserver_wrk_compare.sh
```

## Affinity Verification

If you want to compare scheduler behavior with worker pinning:

```bash
HELLO_HTTP_WORKERS=16 \
HELLO_HTTP_WORKER_AFFINITY=off \
HELLO_HTTP_LOG_LEVEL=off \
./scripts/run_hello_http_wrk.sh medium
```

```bash
HELLO_HTTP_WORKERS=16 \
HELLO_HTTP_WORKER_AFFINITY=physical \
HELLO_HTTP_LOG_LEVEL=off \
./scripts/run_hello_http_wrk.sh medium
```

```bash
HELLO_HTTP_WORKERS=16 \
HELLO_HTTP_WORKER_AFFINITY=logical \
HELLO_HTTP_LOG_LEVEL=off \
./scripts/run_hello_http_wrk.sh medium
```

On SMT systems, `physical` usually pins workers to one sibling per core first,
while `logical` walks every online CPU in OS order.

## Results To Collect

Keep these directories:

- `benchmark-results/wrk/`
- `benchmark-results/wrk-matrix/`
- `benchmark-results/wrk-thread-matrix/`
- `benchmark-results/wrk-compare/`
- `benchmark-results/host-info/`

Each benchmark directory already includes structured outputs such as:

- `report.md`
- `summary.csv`
- raw `*.wrk.txt`
- configuration metadata

## Sharing Results Back

Bundle the host metadata and benchmark outputs:

```bash
tar -czf benchmark-results-$(hostname)-$(date +%Y%m%d-%H%M%S).tar.gz \
  benchmark-results/host-info \
  benchmark-results/wrk \
  benchmark-results/wrk-matrix \
  benchmark-results/wrk-thread-matrix \
  benchmark-results/wrk-compare
```

When comparing machines, record at least:

- machine name
- OS and kernel
- whether it is WSL2, VM, or bare metal
- CPU model and logical CPU count
- the tested commit SHA
- whether `HELLO_HTTP_WORKER_AFFINITY` was `off`, `physical`, or `logical`

## Comparison Tips

When reading cross-machine results:

- trust trends more than a single top-line `req/s`
- compare both throughput and timeout behavior
- compare scheduler-sensitive runs with and without affinity
- keep the same `wrk -t`, `-c`, duration, and tested commit

If one machine is WSL2 and the other is bare metal, treat the comparison as a
platform study rather than a pure runtime regression check.
