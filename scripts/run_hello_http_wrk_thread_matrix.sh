#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-web-bench"}"
TARGET="${TARGET:-hello_http}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
RESULTS_BASE_DIR="${RESULTS_BASE_DIR:-"$ROOT_DIR/benchmark-results/wrk-thread-matrix"}"
TIMESTAMP="${TIMESTAMP:-$(date +%Y%m%d-%H%M%S)}"
RESULTS_DIR="${RESULTS_BASE_DIR}/${TIMESTAMP}"
SERVER_PID=""
MATRIX_MODE="${MATRIX_MODE:-quick}"
LOGICAL_CPUS="${LOGICAL_CPUS:-$(nproc)}"
WORKER_VALUES="${WORKER_VALUES:-1,2,4,8,16,32}"
WRK_THREAD_VALUES="${WRK_THREAD_VALUES:-1,2,4,8,16,32}"
CONNECTIONS_PER_WRK_THREAD="${CONNECTIONS_PER_WRK_THREAD:-32}"
MIN_CONNECTIONS="${MIN_CONNECTIONS:-128}"
MAX_WRK_THREADS="${MAX_WRK_THREADS:-$LOGICAL_CPUS}"
MAX_WORKERS="${MAX_WORKERS:-$LOGICAL_CPUS}"

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

require_cmd wrk
require_cmd curl
require_cmd python3

mkdir -p "$RESULTS_DIR"

if [[ "$MATRIX_MODE" == "quick" ]]; then
    DURATION="${DURATION:-6s}"
else
    DURATION="${DURATION:-15s}"
fi

clamp_value() {
    local value="$1"
    local min_value="$2"
    local max_value="$3"
    if (( value < min_value )); then
        value="$min_value"
    fi
    if (( value > max_value )); then
        value="$max_value"
    fi
    printf '%s\n' "$value"
}

parse_csv_values() {
    local raw="$1"
    local min_value="$2"
    local max_value="$3"
    local -a values=()
    local seen=","
    local item
    IFS=',' read -ra items <<<"$raw"
    for item in "${items[@]}"; do
        [[ -n "$item" ]] || continue
        item="$(clamp_value "$item" "$min_value" "$max_value")"
        if [[ "$seen" == *",$item,"* ]]; then
            continue
        fi
        values+=("$item")
        seen+="$item,"
    done
    printf '%s\n' "${values[@]}"
}

readarray -t WORKERS < <(parse_csv_values "$WORKER_VALUES" 1 "$MAX_WORKERS")
readarray -t WRK_THREADS < <(parse_csv_values "$WRK_THREAD_VALUES" 1 "$MAX_WRK_THREADS")

echo "[wrk-thread-matrix] configuring build in $BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF >/dev/null

echo "[wrk-thread-matrix] building $TARGET"
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc)" >/dev/null

cat >"${RESULTS_DIR}/environment.txt" <<EOF
matrix_mode=${MATRIX_MODE}
host=${HOST}
port=${PORT}
target=${TARGET}
build_dir=${BUILD_DIR}
logical_cpus=${LOGICAL_CPUS}
worker_values=${WORKER_VALUES}
wrk_thread_values=${WRK_THREAD_VALUES}
connections_per_wrk_thread=${CONNECTIONS_PER_WRK_THREAD}
min_connections=${MIN_CONNECTIONS}
duration=${DURATION}
wrk_path=$(command -v wrk)
python3=$(python3 --version 2>&1)
timestamp=${TIMESTAMP}
EOF

for workers in "${WORKERS[@]}"; do
    config_name="workers_${workers}"
    config_dir="${RESULTS_DIR}/${config_name}"
    mkdir -p "$config_dir"

    cat >"${config_dir}/config.meta.json" <<EOF
{
  "config": "${config_name}",
  "workers": ${workers},
  "description": "server worker sweep (${workers} workers)"
}
EOF

    echo "[wrk-thread-matrix] starting ${config_name}"
    HELLO_HTTP_PORT="$PORT" \
    HELLO_HTTP_WORKERS="$workers" \
    HELLO_HTTP_LOG_LEVEL="${HELLO_HTTP_LOG_LEVEL:-off}" \
    "$BUILD_DIR/bin/$TARGET" >"${config_dir}/server.log" 2>&1 &
    SERVER_PID="$!"

    HEALTH_URL="http://${HOST}:${PORT}/health"
    for _ in $(seq 1 50); do
        if curl -fsS "$HEALTH_URL" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done

    if ! curl -fsS "$HEALTH_URL" >/dev/null 2>&1; then
        echo "[wrk-thread-matrix] server failed health check for ${config_name}: $HEALTH_URL" >&2
        tail -n 50 "${config_dir}/server.log" >&2 || true
        exit 1
    fi

    for wrk_threads in "${WRK_THREADS[@]}"; do
        connections=$(( wrk_threads * CONNECTIONS_PER_WRK_THREAD ))
        if (( connections < MIN_CONNECTIONS )); then
            connections="$MIN_CONNECTIONS"
        fi

        name="wrk_t${wrk_threads}"
        raw_output="${name}.wrk.txt"
        meta_output="${name}.meta.json"
        url="http://${HOST}:${PORT}/"

        echo "[wrk-thread-matrix] ${config_name} -> ${name}: wrk_threads=${wrk_threads} connections=${connections} duration=${DURATION}"
        wrk -t"${wrk_threads}" -c"${connections}" -d"${DURATION}" --latency "$url" \
            | tee "${config_dir}/${raw_output}"

        cat >"${config_dir}/${meta_output}" <<EOF
{
  "config": "${config_name}",
  "name": "${name}",
  "workers": ${workers},
  "wrk_threads": ${wrk_threads},
  "connections": ${connections},
  "duration": "${DURATION}",
  "path": "/",
  "url": "${url}",
  "description": "wrk thread sweep",
  "raw_output": "${raw_output}"
}
EOF
    done

    cleanup
done

python3 "${ROOT_DIR}/scripts/wrk/analyze_wrk_thread_matrix.py" "$RESULTS_DIR"

echo "[wrk-thread-matrix] results saved to $RESULTS_DIR"
echo "[wrk-thread-matrix] summary report: ${RESULTS_DIR}/report.md"
