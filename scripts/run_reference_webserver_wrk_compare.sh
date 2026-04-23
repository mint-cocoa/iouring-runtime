#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-web-bench"}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-18080}"
RESULTS_BASE_DIR="${RESULTS_BASE_DIR:-"$ROOT_DIR/benchmark-results/wrk-compare"}"
TIMESTAMP="${TIMESTAMP:-$(date +%Y%m%d-%H%M%S)}"
RESULTS_DIR="${RESULTS_BASE_DIR}/${TIMESTAMP}"
TARGET="${TARGET:-hello_http}"
MODE="${MODE:-standard}"
SERVER_FILTER="${SERVER_FILTER:-}"
CURRENT_LOCAL_PID=""
CURRENT_CONTAINER=""
WARMUP_DURATION="${WARMUP_DURATION:-2s}"

cleanup() {
    if [[ -n "$CURRENT_LOCAL_PID" ]] && kill -0 "$CURRENT_LOCAL_PID" 2>/dev/null; then
        kill "$CURRENT_LOCAL_PID" 2>/dev/null || true
        wait "$CURRENT_LOCAL_PID" 2>/dev/null || true
    fi
    CURRENT_LOCAL_PID=""

    if [[ -n "$CURRENT_CONTAINER" ]]; then
        docker rm -f "$CURRENT_CONTAINER" >/dev/null 2>&1 || true
    fi
    CURRENT_CONTAINER=""
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
require_cmd docker

mkdir -p "$RESULTS_DIR"

if [[ "$MODE" == "quick" ]]; then
    STEADY_DURATION="${STEADY_DURATION:-8s}"
    MIXED_DURATION="${MIXED_DURATION:-8s}"
    SATURATION_DURATION="${SATURATION_DURATION:-10s}"
else
    STEADY_DURATION="${STEADY_DURATION:-10s}"
    MIXED_DURATION="${MIXED_DURATION:-10s}"
    SATURATION_DURATION="${SATURATION_DURATION:-15s}"
fi

WORKLOADS=(
  "steady_root|4|128|${STEADY_DURATION}|/||balanced throughput check on GET /"
  "mixed_routes|4|128|${MIXED_DURATION}|/|${ROOT_DIR}/scripts/wrk/mixed_routes.lua|mixed GET traffic across / and /health"
  "saturation_root|8|256|${SATURATION_DURATION}|/||higher connection pressure on GET /"
)

SERVERS=(
  "iouring_runtime|local|native iouring runtime http server"
  "nginx|docker|nginx official container"
  "caddy|docker|caddy official container"
)

server_selected() {
    local name="$1"
    if [[ -z "$SERVER_FILTER" ]]; then
        return 0
    fi
    case ",${SERVER_FILTER}," in
        *,"${name}",*) return 0 ;;
        *) return 1 ;;
    esac
}

wait_for_health() {
    local url="$1"
    for _ in $(seq 1 80); do
        if curl -fsS "$url" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

warm_up_server() {
    local url="$1"
    wrk -t2 -c16 -d"${WARMUP_DURATION}" "$url" >/dev/null
}

start_iouring_runtime() {
    local server_dir="$1"
    HELLO_HTTP_PORT="$PORT" \
    HELLO_HTTP_WORKERS="${HELLO_HTTP_WORKERS:-4}" \
    HELLO_HTTP_LOG_LEVEL="${HELLO_HTTP_LOG_LEVEL:-info}" \
    "$BUILD_DIR/bin/$TARGET" >"${server_dir}/server.log" 2>&1 &
    CURRENT_LOCAL_PID="$!"
}

start_nginx() {
    local server_dir="$1"
    CURRENT_CONTAINER="wrk-compare-nginx-${TIMESTAMP}"
    docker run --rm -d \
        --name "$CURRENT_CONTAINER" \
        -p "${PORT}:8080" \
        -v "${ROOT_DIR}/scripts/wrk/reference_servers/nginx.conf:/etc/nginx/nginx.conf:ro" \
        nginx:alpine >/dev/null
    docker logs "$CURRENT_CONTAINER" >"${server_dir}/server.log" 2>&1 || true
}

start_caddy() {
    local server_dir="$1"
    CURRENT_CONTAINER="wrk-compare-caddy-${TIMESTAMP}"
    docker run --rm -d \
        --name "$CURRENT_CONTAINER" \
        -p "${PORT}:8080" \
        -v "${ROOT_DIR}/scripts/wrk/reference_servers/Caddyfile:/etc/caddy/Caddyfile:ro" \
        caddy:alpine >/dev/null
    docker logs "$CURRENT_CONTAINER" >"${server_dir}/server.log" 2>&1 || true
}

stop_server() {
    local server_dir="$1"
    if [[ -n "$CURRENT_CONTAINER" ]]; then
        docker logs "$CURRENT_CONTAINER" >"${server_dir}/server.log" 2>&1 || true
    fi
    cleanup
}

echo "[wrk-compare] configuring build in $BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF >/dev/null

echo "[wrk-compare] building $TARGET"
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc)" >/dev/null

echo "[wrk-compare] pulling container images"
docker pull nginx:alpine >/dev/null
docker pull caddy:alpine >/dev/null

cat >"${RESULTS_DIR}/environment.txt" <<EOF
mode=${MODE}
host=${HOST}
port=${PORT}
target=${TARGET}
build_dir=${BUILD_DIR}
wrk_path=$(command -v wrk)
docker_version=$(docker version --format '{{.Server.Version}}')
python3=$(python3 --version 2>&1)
timestamp=${TIMESTAMP}
EOF

for server_entry in "${SERVERS[@]}"; do
    IFS='|' read -r name kind description <<<"$server_entry"
    if ! server_selected "$name"; then
        continue
    fi

    server_dir="${RESULTS_DIR}/${name}"
    mkdir -p "$server_dir"
    echo "[wrk-compare] starting ${name}"

    case "$name" in
        iouring_runtime)
            start_iouring_runtime "$server_dir"
            ;;
        nginx)
            start_nginx "$server_dir"
            ;;
        caddy)
            start_caddy "$server_dir"
            ;;
        *)
            echo "unknown server: $name" >&2
            exit 1
            ;;
    esac

    if ! wait_for_health "http://${HOST}:${PORT}/health"; then
        echo "[wrk-compare] ${name} failed health check" >&2
        tail -n 80 "${server_dir}/server.log" >&2 || true
        exit 1
    fi

    echo "[wrk-compare] warming up ${name} for ${WARMUP_DURATION}"
    warm_up_server "http://${HOST}:${PORT}/"

    image_ref=""
    if [[ "$kind" == "docker" ]]; then
        image_ref="$(docker inspect --format '{{.Config.Image}}' "$CURRENT_CONTAINER")"
    fi

    cat >"${server_dir}/server.meta.json" <<EOF
{
  "name": "${name}",
  "kind": "${kind}",
  "description": "${description}",
  "image": "${image_ref}"
}
EOF

    for workload in "${WORKLOADS[@]}"; do
        IFS='|' read -r workload_name threads connections duration path script workload_description <<<"$workload"
        url="http://${HOST}:${PORT}${path}"
        raw_output="${workload_name}.wrk.txt"
        meta_output="${workload_name}.meta.json"

        echo "[wrk-compare] ${name} -> ${workload_name}: threads=${threads} conns=${connections} duration=${duration}"
        cmd=(wrk -t"${threads}" -c"${connections}" -d"${duration}" --latency)
        if [[ -n "$script" ]]; then
            cmd+=(-s "$script")
        fi
        cmd+=("$url")
        "${cmd[@]}" | tee "${server_dir}/${raw_output}"

        cat >"${server_dir}/${meta_output}" <<EOF
{
  "server": "${name}",
  "name": "${workload_name}",
  "threads": ${threads},
  "connections": ${connections},
  "duration": "${duration}",
  "path": "${path}",
  "url": "${url}",
  "script": "${script}",
  "description": "${workload_description}",
  "raw_output": "${raw_output}"
}
EOF
    done

    stop_server "$server_dir"
done

python3 "${ROOT_DIR}/scripts/wrk/analyze_wrk_compare.py" "$RESULTS_DIR"

echo "[wrk-compare] results saved to $RESULTS_DIR"
echo "[wrk-compare] summary report: ${RESULTS_DIR}/report.md"
