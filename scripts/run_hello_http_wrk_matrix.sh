#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-web-bench"}"
TARGET="${TARGET:-hello_http}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
RESULTS_BASE_DIR="${RESULTS_BASE_DIR:-"$ROOT_DIR/benchmark-results/wrk-matrix"}"
TIMESTAMP="${TIMESTAMP:-$(date +%Y%m%d-%H%M%S)}"
RESULTS_DIR="${RESULTS_BASE_DIR}/${TIMESTAMP}"
SERVER_PID=""
SAMPLER_PID=""
MATRIX_MODE="${MATRIX_MODE:-quick}"
CONFIG_FILTER="${CONFIG_FILTER:-}"
WORKER_SWEEP_MODE="${WORKER_SWEEP_MODE:-auto}"
CPU_SAMPLE_INTERVAL="${CPU_SAMPLE_INTERVAL:-0.5}"
LOGICAL_CPUS="${LOGICAL_CPUS:-$(nproc)}"
MAX_AUTO_WORKERS="${MAX_AUTO_WORKERS:-$LOGICAL_CPUS}"
AUTO_TUNED_WORKERS="${AUTO_TUNED_WORKERS:-4}"

cleanup() {
    if [[ -n "$SAMPLER_PID" ]] && kill -0 "$SAMPLER_PID" 2>/dev/null; then
        kill "$SAMPLER_PID" 2>/dev/null || true
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi
    SAMPLER_PID=""
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

clamp_workers() {
    local value="$1"
    if (( value < 1 )); then
        value=1
    fi
    if (( value > LOGICAL_CPUS )); then
        value="$LOGICAL_CPUS"
    fi
    if (( value > MAX_AUTO_WORKERS )); then
        value="$MAX_AUTO_WORKERS"
    fi
    printf '%s\n' "$value"
}

build_worker_sweep() {
    local -a values=()
    local current=1
    while (( current <= MAX_AUTO_WORKERS && current <= LOGICAL_CPUS )); do
        values+=("$current")
        current=$(( current * 2 ))
    done

    local capped_max
    capped_max="$(clamp_workers "$MAX_AUTO_WORKERS")"
    if [[ "${values[*]}" != *"$capped_max"* ]]; then
        values+=("$capped_max")
    fi

    local deduped=()
    local seen=","
    local item
    for item in "${values[@]}"; do
        if [[ "$seen" == *",$item,"* ]]; then
            continue
        fi
        deduped+=("$item")
        seen+="$item,"
    done
    printf '%s\n' "${deduped[@]}"
}

start_cpu_sampler() {
    local csv_path="$1"
    local summary_path="$2"
    python3 "${ROOT_DIR}/scripts/wrk/sample_cpu_usage.py" \
        --pid "$SERVER_PID" \
        --interval "$CPU_SAMPLE_INTERVAL" \
        --csv "$csv_path" \
        --summary "$summary_path" &
    SAMPLER_PID="$!"
}

stop_cpu_sampler() {
    if [[ -n "$SAMPLER_PID" ]] && kill -0 "$SAMPLER_PID" 2>/dev/null; then
        kill "$SAMPLER_PID" 2>/dev/null || true
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi
    SAMPLER_PID=""
}

if [[ "$MATRIX_MODE" == "quick" ]]; then
    STEADY_DURATION="${STEADY_DURATION:-8s}"
    SATURATION_DURATION="${SATURATION_DURATION:-10s}"
else
    STEADY_DURATION="${STEADY_DURATION:-20s}"
    SATURATION_DURATION="${SATURATION_DURATION:-20s}"
fi

WORKLOADS=(
  "steady_root|4|128|${STEADY_DURATION}|/|balanced throughput check on GET /"
  "saturation_root|8|256|${SATURATION_DURATION}|/|higher connection pressure on GET /"
)

AUTO_TUNED_WORKERS="$(clamp_workers "$AUTO_TUNED_WORKERS")"

CONFIGS=()
if [[ "$WORKER_SWEEP_MODE" == "auto" ]]; then
    while IFS= read -r workers; do
        [[ -n "$workers" ]] || continue
        CONFIGS+=(
          "baseline_w${workers}|${workers}|2048|4096|1|0|1|auto worker baseline (${workers} workers)"
        )
    done < <(build_worker_sweep)
    CONFIGS+=(
      "batching_w${AUTO_TUNED_WORKERS}|${AUTO_TUNED_WORKERS}|2048|4096|8|64|1|${AUTO_TUNED_WORKERS} workers with batched submit and CQE processing"
      "deep_queue_w${AUTO_TUNED_WORKERS}|${AUTO_TUNED_WORKERS}|4096|8192|1|0|1|${AUTO_TUNED_WORKERS} workers with deeper queue depth and buffers"
      "combined_tuned_w${AUTO_TUNED_WORKERS}|${AUTO_TUNED_WORKERS}|4096|8192|8|64|1|${AUTO_TUNED_WORKERS} workers with deeper queue and batching"
      "spin_poll_w${AUTO_TUNED_WORKERS}|${AUTO_TUNED_WORKERS}|2048|4096|1|0|0|${AUTO_TUNED_WORKERS} workers with 0ms io_timeout busy polling"
    )
else
    CONFIGS=(
      "baseline_w1|1|2048|4096|1|0|1|single worker baseline"
      "baseline_w2|2|2048|4096|1|0|1|two workers baseline"
      "baseline_w4|4|2048|4096|1|0|1|four workers baseline"
      "baseline_w8|8|2048|4096|1|0|1|eight workers baseline"
      "batching_w4|4|2048|4096|8|64|1|four workers with batched submit and CQE processing"
      "deep_queue_w4|4|4096|8192|1|0|1|four workers with deeper queue depth and buffers"
      "combined_tuned_w4|4|4096|8192|8|64|1|four workers with deeper queue and batching"
      "spin_poll_w4|4|2048|4096|1|0|0|four workers with 0ms io_timeout busy polling"
    )
fi

echo "[wrk-matrix] configuring build in $BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF >/dev/null

echo "[wrk-matrix] building $TARGET"
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc)" >/dev/null

cat >"${RESULTS_DIR}/environment.txt" <<EOF
matrix_mode=${MATRIX_MODE}
host=${HOST}
port=${PORT}
target=${TARGET}
build_dir=${BUILD_DIR}
logical_cpus=${LOGICAL_CPUS}
worker_sweep_mode=${WORKER_SWEEP_MODE}
max_auto_workers=${MAX_AUTO_WORKERS}
auto_tuned_workers=${AUTO_TUNED_WORKERS}
cpu_sample_interval=${CPU_SAMPLE_INTERVAL}
wrk_path=$(command -v wrk)
python3=$(python3 --version 2>&1)
timestamp=${TIMESTAMP}
EOF

for config_entry in "${CONFIGS[@]}"; do
    IFS='|' read -r config_name workers queue_depth buf_count submit_batch cqe_budget io_timeout_ms description <<<"$config_entry"
    if [[ -n "$CONFIG_FILTER" ]]; then
        case ",${CONFIG_FILTER}," in
            *,"${config_name}",*) ;;
            *) continue ;;
        esac
    fi
    config_dir="${RESULTS_DIR}/${config_name}"
    mkdir -p "$config_dir"

    cat >"${config_dir}/config.meta.json" <<EOF
{
  "name": "${config_name}",
  "description": "${description}",
  "workers": ${workers},
  "ring_queue_depth": ${queue_depth},
  "ring_buf_count": ${buf_count},
  "ring_submit_batch_size": ${submit_batch},
  "ring_cqe_batch_budget": ${cqe_budget},
  "ring_io_timeout_ms": ${io_timeout_ms},
  "logical_cpus": ${LOGICAL_CPUS}
}
EOF

    echo "[wrk-matrix] starting ${config_name}: workers=${workers} qd=${queue_depth} buf_count=${buf_count} submit_batch=${submit_batch} cqe_budget=${cqe_budget} io_timeout_ms=${io_timeout_ms}"
    HELLO_HTTP_PORT="$PORT" \
    HELLO_HTTP_WORKERS="$workers" \
    HELLO_HTTP_LOG_LEVEL="${HELLO_HTTP_LOG_LEVEL:-info}" \
    HELLO_HTTP_RING_QUEUE_DEPTH="$queue_depth" \
    HELLO_HTTP_RING_BUF_COUNT="$buf_count" \
    HELLO_HTTP_RING_SUBMIT_BATCH="$submit_batch" \
    HELLO_HTTP_RING_CQE_BATCH_BUDGET="$cqe_budget" \
    HELLO_HTTP_RING_IO_TIMEOUT_MS="$io_timeout_ms" \
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
        echo "[wrk-matrix] server failed health check for ${config_name}: $HEALTH_URL" >&2
        tail -n 50 "${config_dir}/server.log" >&2 || true
        exit 1
    fi

    for workload in "${WORKLOADS[@]}"; do
        IFS='|' read -r workload_name threads connections duration path workload_description <<<"$workload"
        url="http://${HOST}:${PORT}${path}"
        raw_output="${workload_name}.wrk.txt"
        meta_output="${workload_name}.meta.json"
        cpu_csv="${workload_name}.cpu.csv"
        cpu_summary="${workload_name}.cpu.summary.json"

        echo "[wrk-matrix] ${config_name} -> ${workload_name}: threads=${threads} conns=${connections} duration=${duration}"
        start_cpu_sampler "${config_dir}/${cpu_csv}" "${config_dir}/${cpu_summary}"
        wrk -t"${threads}" -c"${connections}" -d"${duration}" --latency "$url" \
            | tee "${config_dir}/${raw_output}"
        stop_cpu_sampler

        cat >"${config_dir}/${meta_output}" <<EOF
{
  "config": "${config_name}",
  "name": "${workload_name}",
  "threads": ${threads},
  "connections": ${connections},
  "duration": "${duration}",
  "path": "${path}",
  "url": "${url}",
  "description": "${workload_description}",
  "raw_output": "${raw_output}",
  "cpu_csv": "${cpu_csv}",
  "cpu_summary": "${cpu_summary}"
}
EOF
    done

    cleanup
done

python3 "${ROOT_DIR}/scripts/wrk/analyze_wrk_matrix.py" "$RESULTS_DIR"

echo "[wrk-matrix] results saved to $RESULTS_DIR"
echo "[wrk-matrix] summary report: ${RESULTS_DIR}/report.md"
