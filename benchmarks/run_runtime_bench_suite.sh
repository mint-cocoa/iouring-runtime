#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-bench"}"
RESULTS_BASE_DIR="${RESULTS_BASE_DIR:-"$ROOT_DIR/benchmark-results/runtime-suite"}"
TIMESTAMP="${TIMESTAMP:-$(date +%Y%m%d-%H%M%S)}"
RESULTS_DIR="${RESULTS_BASE_DIR}/${TIMESTAMP}"
HOST="${HOST:-127.0.0.1}"
SUITE_MODE="${SUITE_MODE:-${MODE:-standard}}"
SERVER_PID=""

if [[ "$SUITE_MODE" == "quick" ]]; then
    REPEATS="${REPEATS:-1}"
    ECHO_DURATION="${ECHO_DURATION:-3}"
    GAME_DURATION="${GAME_DURATION:-3}"
    ECHO_CLIENTS=(${ECHO_CLIENTS:-10 50})
    GAME_SCENARIOS=(${GAME_SCENARIOS:-"40:1" "80:4"})
    SERVER_THREADS=(${SERVER_THREADS:-1 4})
else
    REPEATS="${REPEATS:-3}"
    ECHO_DURATION="${ECHO_DURATION:-10}"
    GAME_DURATION="${GAME_DURATION:-10}"
    ECHO_CLIENTS=(${ECHO_CLIENTS:-10 50 100 200})
    GAME_SCENARIOS=(${GAME_SCENARIOS:-"40:1" "200:1" "400:1" "200:10" "400:20"})
    SERVER_THREADS=(${SERVER_THREADS:-1 4 8})
fi

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
}
trap cleanup EXIT

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "$1 not found in PATH" >&2
        exit 1
    fi
}

wait_for_port() {
    local host="$1"
    local port="$2"
    python3 - "$host" "$port" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
for _ in range(100):
    try:
        with socket.create_connection((host, port), timeout=0.2):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY
}

start_server() {
    local name="$1"
    local port="$2"
    shift 2
    cleanup
    echo "[runtime-suite] starting ${name} on ${HOST}:${port}"
    "$@" >"${RESULTS_DIR}/${name}.log" 2>&1 &
    SERVER_PID="$!"
    if ! wait_for_port "$HOST" "$port"; then
        echo "[runtime-suite] ${name} failed to open port ${port}" >&2
        tail -n 80 "${RESULTS_DIR}/${name}.log" >&2 || true
        exit 1
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[runtime-suite] ${name} exited while waiting for port ${port}" >&2
        tail -n 80 "${RESULTS_DIR}/${name}.log" >&2 || true
        exit 1
    fi
}

run_echo_group() {
    local server_name="$1"
    local port="$2"
    local threads="$3"
    for clients in "${ECHO_CLIENTS[@]}"; do
        for repeat in $(seq 1 "$REPEATS"); do
            local out="${RESULTS_DIR}/${server_name}_echo_t${threads}_c${clients}_r${repeat}.json"
            echo "[runtime-suite] echo ${server_name} threads=${threads} clients=${clients} repeat=${repeat}"
            "$BUILD_DIR/bin/runtime_echo_bench" \
                --host "$HOST" \
                --port "$port" \
                --clients "$clients" \
                --payload 64 \
                --pipeline 1 \
                --duration "$ECHO_DURATION" | tee "$out"
        done
    done
}

run_game_group() {
    local server_name="$1"
    local port="$2"
    local threads="$3"
    for scenario in "${GAME_SCENARIOS[@]}"; do
        IFS=':' read -r bots rooms <<<"$scenario"
        for repeat in $(seq 1 "$REPEATS"); do
            local out="${RESULTS_DIR}/${server_name}_game_t${threads}_b${bots}_rooms${rooms}_r${repeat}.json"
            echo "[runtime-suite] game ${server_name} threads=${threads} bots=${bots} rooms=${rooms} repeat=${repeat}"
            "$BUILD_DIR/bin/runtime_game_fanout_bench" \
                --host "$HOST" \
                --port "$port" \
                --bots "$bots" \
                --rooms "$rooms" \
                --move-hz 20 \
                --attack-hz 0 \
                --duration "$GAME_DURATION" | tee "$out"
        done
    done
}

run_wrk_probe() {
    local name="$1"
    local url="$2"
    if ! command -v wrk >/dev/null 2>&1; then
        echo "[runtime-suite] wrk not found; skipping ${name}" | tee "${RESULTS_DIR}/${name}.wrk.txt"
        return
    fi
    echo "[runtime-suite] wrk ${name}"
    wrk -t2 -c32 -d"${WRK_DURATION:-5s}" --latency "$url" | tee "${RESULTS_DIR}/${name}.wrk.txt"
}

require_cmd cmake
require_cmd python3

mkdir -p "$RESULTS_DIR"

echo "[runtime-suite] configuring ${BUILD_DIR}"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_STREAM=ON \
  -DBUILD_GAME=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_BENCHMARKS=ON \
  -DBUILD_TESTS=ON

echo "[runtime-suite] building benchmark targets"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "[runtime-suite] running ctest"
ctest --test-dir "$BUILD_DIR" --output-on-failure | tee "${RESULTS_DIR}/ctest.txt"

{
    echo "timestamp=${TIMESTAMP}"
    echo "suite_mode=${SUITE_MODE}"
    echo "root=${ROOT_DIR}"
    echo "build_dir=${BUILD_DIR}"
    echo "commit=$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || true)"
    echo "status=$(git -C "$ROOT_DIR" status --short 2>/dev/null | wc -l)"
    echo "uname=$(uname -a)"
    echo "kernel=$(cat /proc/sys/kernel/osrelease 2>/dev/null || true)"
    echo "wsl2=$(grep -qi microsoft /proc/version /proc/sys/kernel/osrelease 2>/dev/null && echo yes || echo no)"
    echo "cmake=$(cmake --version | head -n 1)"
    echo "python3=$(python3 --version 2>&1)"
    echo
    lscpu 2>/dev/null || true
} >"${RESULTS_DIR}/environment.txt"

for threads in "${SERVER_THREADS[@]}"; do
    start_server "core_echo_t${threads}" 29090 env \
        CORE_ECHO_PORT=29090 \
        CORE_ECHO_WORKERS="$threads" \
        "$BUILD_DIR/bin/core_echo"
    run_echo_group "iouring" 29090 "$threads"

    start_server "epoll_echo_server_t${threads}" 29091 env \
        EPOLL_ECHO_PORT=29091 \
        EPOLL_ECHO_WORKERS="$threads" \
        "$BUILD_DIR/bin/epoll_echo_server"
    run_echo_group "epoll" 29091 "$threads"

    start_server "iouring_game_fanout_server_t${threads}" 29120 env \
        IOURING_FANOUT_PORT=29120 \
        IOURING_FANOUT_WORKERS="$threads" \
        "$BUILD_DIR/bin/iouring_game_fanout_server"
    run_game_group "iouring" 29120 "$threads"

    start_server "epoll_game_fanout_server_t${threads}" 29121 env \
        EPOLL_FANOUT_PORT=29121 \
        EPOLL_FANOUT_WORKERS="$threads" \
        "$BUILD_DIR/bin/epoll_game_fanout_server"
    run_game_group "epoll" 29121 "$threads"
done

start_server "hello_http" 28080 env \
    HELLO_HTTP_PORT=28080 \
    HELLO_HTTP_WORKERS="${WEB_THREADS:-${SERVER_THREADS[0]}}" \
    HELLO_HTTP_LOG_LEVEL=off \
    HELLO_HTTP_STATIC_ROOT="$ROOT_DIR/benchmarks/wrk/reference_servers/www" \
    "$BUILD_DIR/bin/hello_http"
run_wrk_probe "web_iouring_root" "http://${HOST}:28080/"
run_wrk_probe "web_iouring_health" "http://${HOST}:28080/health"

start_server "epoll_http_server" 28081 env EPOLL_HTTP_PORT=28081 "$BUILD_DIR/bin/epoll_http_server"
run_wrk_probe "web_epoll_root" "http://${HOST}:28081/"
run_wrk_probe "web_epoll_health" "http://${HOST}:28081/health"

start_server "hello_http_upstream" 28083 env \
    HELLO_HTTP_PORT=28083 \
    HELLO_HTTP_WORKERS="${WEB_THREADS:-${SERVER_THREADS[0]}}" \
    HELLO_HTTP_LOG_LEVEL=off \
    HELLO_HTTP_STATIC_ROOT="$ROOT_DIR/benchmarks/wrk/reference_servers/www" \
    "$BUILD_DIR/bin/hello_http"
UPSTREAM_PID="$SERVER_PID"
SERVER_PID=""

start_server "tcp_reverse_proxy" 28084 env \
    TCP_PROXY_LISTEN_PORT=28084 \
    TCP_PROXY_UPSTREAM_HOST=127.0.0.1 \
    TCP_PROXY_UPSTREAM_PORT=28083 \
    TCP_PROXY_WORKERS="${PROXY_THREADS:-${SERVER_THREADS[0]}}" \
    TCP_PROXY_LOG_LEVEL=off \
    "$BUILD_DIR/bin/tcp_reverse_proxy"
PROXY_PID="$SERVER_PID"
run_wrk_probe "proxy_iouring_root" "http://${HOST}:28084/"
kill "$PROXY_PID" 2>/dev/null || true
wait "$PROXY_PID" 2>/dev/null || true
SERVER_PID=""

start_server "epoll_tcp_proxy" 28085 env \
    EPOLL_PROXY_PORT=28085 \
    EPOLL_PROXY_UPSTREAM_HOST=127.0.0.1 \
    EPOLL_PROXY_UPSTREAM_PORT=28083 \
    "$BUILD_DIR/bin/epoll_tcp_proxy"
run_wrk_probe "proxy_epoll_root" "http://${HOST}:28085/"
cleanup
kill "$UPSTREAM_PID" 2>/dev/null || true
wait "$UPSTREAM_PID" 2>/dev/null || true

if [[ -x "$BUILD_DIR/bin/dungeon_full_server" && -x "$BUILD_DIR/bin/dungeon_protocol_probe" ]]; then
    start_server "dungeon_full_server" 27777 env \
        DUNGEON_SERVER_PORT=27777 \
        DUNGEON_SERVER_WORKERS=2 \
        GAMESERVER_DB=memory \
        "$BUILD_DIR/bin/dungeon_full_server"
    "$BUILD_DIR/bin/dungeon_protocol_probe" "$HOST" 27777 | tee "${RESULTS_DIR}/dungeon_protocol_probe.txt"
    cleanup
fi

python3 - "$RESULTS_DIR" <<'PY'
import csv
import json
import statistics
import sys
from pathlib import Path

out = Path(sys.argv[1])
rows = []
for path in sorted(out.glob("*.json")):
    data = json.loads(path.read_text())
    stem = path.stem
    parts = stem.split("_")
    server = parts[0]
    server_threads = "1"
    for part in parts:
        if part.startswith("t") and part[1:].isdigit():
            server_threads = part[1:]
            break
    bench_type = data.get("type", "")
    scenario = ""
    if bench_type == "echo":
        scenario = f"threads={server_threads},clients={data['clients']},payload={data['payload_bytes']}"
        throughput = data["throughput_per_sec"]
        messages = data["completed"]
    else:
        scenario = f"threads={server_threads},bots={data['bots']},rooms={data['rooms']},move_hz={data['move_hz']}"
        throughput = data["messages_per_sec"]
        messages = data["received_messages"]
    rows.append({
        "file": path.name,
        "server": server,
        "type": bench_type,
        "scenario": scenario,
        "repeat": stem.rsplit("_r", 1)[-1] if "_r" in stem else "1",
        "throughput": throughput,
        "p50_us": data.get("p50_us", 0),
        "p90_us": data.get("p90_us", 0),
        "p99_us": data.get("p99_us", 0),
        "messages": messages,
        "errors": data.get("errors", 0),
        "missing_ratio": data.get("missing_ratio", 0),
    })

with (out / "summary.csv").open("w", newline="") as handle:
    writer = csv.DictWriter(handle, fieldnames=[
        "file", "server", "type", "scenario", "repeat", "throughput",
        "p50_us", "p90_us", "p99_us", "messages", "errors", "missing_ratio",
    ])
    writer.writeheader()
    writer.writerows(rows)

groups = {}
for row in rows:
    groups.setdefault((row["server"], row["type"], row["scenario"]), []).append(row)

lines = [
    "# runtime Benchmark Suite Report",
    "",
    "## Summary",
    "",
    "| Server | Type | Scenario | Runs | Median throughput | Median p50 | Median p99 | Errors | Max missing |",
    "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
]
for key, items in sorted(groups.items()):
    server, bench_type, scenario = key
    median_thr = statistics.median(float(i["throughput"]) for i in items)
    median_p50 = statistics.median(float(i["p50_us"]) for i in items)
    median_p99 = statistics.median(float(i["p99_us"]) for i in items)
    errors = sum(int(i["errors"]) for i in items)
    missing = max(float(i["missing_ratio"]) for i in items)
    lines.append(
        f"| {server} | {bench_type} | `{scenario}` | {len(items)} | "
        f"{median_thr:,.2f} | {median_p50:,.0f} us | {median_p99:,.0f} us | "
        f"{errors} | {missing:.4%} |"
    )

lines.extend([
    "",
    "## Notes",
    "",
    "- Echo and game fan-out rows use benchmark JSON output and median values across repeats.",
    "- Web and proxy `wrk` raw outputs are stored next to this report as `*.wrk.txt`.",
    "- `epoll` rows are benchmark-only reference servers, not product feature parity implementations.",
    "- Environment metadata is stored in `environment.txt`.",
])
(out / "report.md").write_text("\n".join(lines) + "\n")
PY

echo "[runtime-suite] results saved to ${RESULTS_DIR}"
echo "[runtime-suite] summary report: ${RESULTS_DIR}/report.md"
