#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-web-bench"}"
TARGET="${TARGET:-hello_http}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
RESULTS_BASE_DIR="${RESULTS_BASE_DIR:-"$ROOT_DIR/benchmark-results/wrk"}"
TIMESTAMP="${TIMESTAMP:-$(date +%Y%m%d-%H%M%S)}"
RESULTS_DIR="${RESULTS_BASE_DIR}/${TIMESTAMP}"
SERVER_PID=""
SUITE_MODE="${SUITE_MODE:-standard}"

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "$1 not found in PATH" >&2
        exit 1
    fi
}

require_cmd wrk
require_cmd curl
require_cmd python3

mkdir -p "$RESULTS_DIR"

if [[ "$SUITE_MODE" == "quick" ]]; then
    SMOKE_DURATION="${SMOKE_DURATION:-5s}"
    STEADY_DURATION="${STEADY_DURATION:-10s}"
    MIXED_DURATION="${MIXED_DURATION:-10s}"
    PAYLOAD_SMALL_DURATION="${PAYLOAD_SMALL_DURATION:-8s}"
    PAYLOAD_MEDIUM_DURATION="${PAYLOAD_MEDIUM_DURATION:-8s}"
    PAYLOAD_LARGE_DURATION="${PAYLOAD_LARGE_DURATION:-10s}"
    SATURATION_DURATION="${SATURATION_DURATION:-12s}"
    SOAK_DURATION="${SOAK_DURATION:-20s}"
else
    SMOKE_DURATION="${SMOKE_DURATION:-10s}"
    STEADY_DURATION="${STEADY_DURATION:-20s}"
    MIXED_DURATION="${MIXED_DURATION:-20s}"
    PAYLOAD_SMALL_DURATION="${PAYLOAD_SMALL_DURATION:-12s}"
    PAYLOAD_MEDIUM_DURATION="${PAYLOAD_MEDIUM_DURATION:-12s}"
    PAYLOAD_LARGE_DURATION="${PAYLOAD_LARGE_DURATION:-15s}"
    SATURATION_DURATION="${SATURATION_DURATION:-20s}"
    SOAK_DURATION="${SOAK_DURATION:-60s}"
fi

SCENARIOS=(
  "smoke_root|2|32|${SMOKE_DURATION}|/||quick sanity check on GET /"
  "steady_root|4|128|${STEADY_DURATION}|/||balanced throughput and latency run on GET /"
  "mixed_routes|4|128|${MIXED_DURATION}|/|${ROOT_DIR}/benchmarks/wrk/mixed_routes.lua|mixed GET traffic across /, /health, and payload sizes"
  "payload_256b|4|128|${PAYLOAD_SMALL_DURATION}|/payload/256b||small payload response"
  "payload_4k|4|96|${PAYLOAD_MEDIUM_DURATION}|/payload/4k||medium payload response"
  "payload_64k|4|32|${PAYLOAD_LARGE_DURATION}|/payload/64k||large payload response"
  "saturation_root|8|256|${SATURATION_DURATION}|/||higher connection pressure on GET /"
  "soak_root|4|128|${SOAK_DURATION}|/||longer stability run on GET /"
)

echo "[wrk-suite] configuring build in $BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF >/dev/null

echo "[wrk-suite] building $TARGET"
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc)" >/dev/null

echo "[wrk-suite] starting $TARGET on ${HOST}:${PORT}"
HELLO_HTTP_PORT="$PORT" \
HELLO_HTTP_LOG_LEVEL="${HELLO_HTTP_LOG_LEVEL:-info}" \
HELLO_HTTP_STATIC_ROOT="${HELLO_HTTP_STATIC_ROOT:-$ROOT_DIR/benchmarks/wrk/reference_servers/www}" \
"$BUILD_DIR/bin/$TARGET" >"$BUILD_DIR/${TARGET}.log" 2>&1 &
SERVER_PID="$!"

HEALTH_URL="http://${HOST}:${PORT}/health"
for _ in $(seq 1 50); do
    if curl -fsS "$HEALTH_URL" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

if ! curl -fsS "$HEALTH_URL" >/dev/null 2>&1; then
    echo "[wrk-suite] server failed health check: $HEALTH_URL" >&2
    tail -n 50 "$BUILD_DIR/${TARGET}.log" >&2 || true
    exit 1
fi

cat >"${RESULTS_DIR}/environment.txt" <<EOF
suite_mode=${SUITE_MODE}
host=${HOST}
port=${PORT}
target=${TARGET}
build_dir=${BUILD_DIR}
wrk_path=$(command -v wrk)
python3=$(python3 --version 2>&1)
timestamp=${TIMESTAMP}
EOF

for scenario in "${SCENARIOS[@]}"; do
    IFS='|' read -r name threads connections duration path script description <<<"$scenario"
    url="http://${HOST}:${PORT}${path}"
    raw_output="${name}.wrk.txt"
    meta_output="${name}.meta.json"

    echo "[wrk-suite] running ${name}: threads=${threads} conns=${connections} duration=${duration} path=${path}"
    cmd=(wrk -t"${threads}" -c"${connections}" -d"${duration}" --latency)
    if [[ -n "$script" ]]; then
        cmd+=(-s "$script")
    fi
    cmd+=("$url")

    "${cmd[@]}" | tee "${RESULTS_DIR}/${raw_output}"

    cat >"${RESULTS_DIR}/${meta_output}" <<EOF
{
  "name": "${name}",
  "threads": ${threads},
  "connections": ${connections},
  "duration": "${duration}",
  "path": "${path}",
  "url": "${url}",
  "script": "${script}",
  "description": "${description}",
  "raw_output": "${raw_output}"
}
EOF
done

python3 "${ROOT_DIR}/benchmarks/wrk/analyze_wrk_results.py" "$RESULTS_DIR"

echo "[wrk-suite] results saved to $RESULTS_DIR"
echo "[wrk-suite] summary report: ${RESULTS_DIR}/report.md"
