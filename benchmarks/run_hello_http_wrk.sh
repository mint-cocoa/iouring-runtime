#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-web-bench"}"
TARGET="${TARGET:-hello_http}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
PATH_SUFFIX="${PATH_SUFFIX:-/}"
PRESET="${1:-medium}"
SERVER_PID=""

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

if ! command -v wrk >/dev/null 2>&1; then
    echo "wrk not found in PATH" >&2
    exit 1
fi

case "$PRESET" in
    light)
        THREADS="${THREADS:-2}"
        CONNECTIONS="${CONNECTIONS:-32}"
        DURATION="${DURATION:-15s}"
        ;;
    medium)
        THREADS="${THREADS:-4}"
        CONNECTIONS="${CONNECTIONS:-128}"
        DURATION="${DURATION:-30s}"
        ;;
    heavy)
        THREADS="${THREADS:-8}"
        CONNECTIONS="${CONNECTIONS:-256}"
        DURATION="${DURATION:-45s}"
        ;;
    soak)
        THREADS="${THREADS:-4}"
        CONNECTIONS="${CONNECTIONS:-128}"
        DURATION="${DURATION:-5m}"
        ;;
    *)
        echo "usage: $0 [light|medium|heavy|soak]" >&2
        exit 1
        ;;
esac

REQUEST_URL="http://${HOST}:${PORT}${PATH_SUFFIX}"
HEALTH_URL="http://${HOST}:${PORT}/health"

echo "[wrk] configuring build in $BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF >/dev/null

echo "[wrk] building $TARGET"
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc)" >/dev/null

echo "[wrk] starting $TARGET on ${HOST}:${PORT}"
HELLO_HTTP_PORT="$PORT" \
HELLO_HTTP_LOG_LEVEL="${HELLO_HTTP_LOG_LEVEL:-info}" \
"$BUILD_DIR/bin/$TARGET" >"$BUILD_DIR/${TARGET}.log" 2>&1 &
SERVER_PID="$!"

for _ in $(seq 1 50); do
    if curl -fsS "$HEALTH_URL" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

if ! curl -fsS "$HEALTH_URL" >/dev/null 2>&1; then
    echo "[wrk] server failed health check: $HEALTH_URL" >&2
    echo "[wrk] last server log lines:" >&2
    tail -n 50 "$BUILD_DIR/${TARGET}.log" >&2 || true
    exit 1
fi

echo "[wrk] preset=$PRESET threads=$THREADS connections=$CONNECTIONS duration=$DURATION"
echo "[wrk] url=$REQUEST_URL"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency "$REQUEST_URL"
