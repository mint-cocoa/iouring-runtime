#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SANITIZER="${1:-asan}"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-${SANITIZER}"}"

case "$SANITIZER" in
  asan|tsan|ubsan|asan+ubsan) ;;
  *)
    echo "usage: $0 [asan|tsan|ubsan|asan+ubsan]" >&2
    exit 1
    ;;
esac

TEST_REGEX="${TEST_REGEX:-test_session_lifecycle|test_session_partial_send|test_cross_thread_flow|test_poll_add|test_timeout}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIORUNTIME_SANITIZER="$SANITIZER"

cmake --build "$BUILD_DIR" -j"$(nproc)"

ctest --test-dir "$BUILD_DIR" --output-on-failure -R "$TEST_REGEX"
