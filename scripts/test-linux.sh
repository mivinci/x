#!/usr/bin/env bash
# test-linux.sh — Build and test libx on Linux
#
# Usage:
#   scripts/test-linux.sh -t <openssl|mbedtls> [-j <jobs>] [-B <build-dir>] [--asan]

set -euo pipefail

TLS_BACKEND="openssl"
JOBS=$(nproc 2>/dev/null || echo 4)
BUILD_DIR="build"
ASAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -t) TLS_BACKEND="$2"; shift 2 ;;
    -j) JOBS="$2"; shift 2 ;;
    -B) BUILD_DIR="$2"; shift 2 ;;
    --asan) ASAN=1; shift ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

# ── ASan configuration ──────────────────────────────────────────────────
if [[ $ASAN -eq 1 ]]; then
  export ASAN_OPTIONS="halt_on_error=0:allocator_may_return_null=1"
  LSAN_SUPPRESSIONS="$(cd "$(dirname "$0")" && pwd)/lsan_suppressions.txt"
  if [[ -f "$LSAN_SUPPRESSIONS" ]]; then
    export LSAN_OPTIONS="suppressions=$LSAN_SUPPRESSIONS"
  fi
  CMAKE_ASAN_FLAGS="-DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer' -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'"
else
  CMAKE_ASAN_FLAGS=""
fi

# ── Build ───────────────────────────────────────────────────────────────
echo "=== Configuring (TLS: $TLS_BACKEND, jobs: $JOBS) ==="
eval cmake -B "$BUILD_DIR" -DX_TLS_BACKEND="$TLS_BACKEND" $CMAKE_ASAN_FLAGS

echo "=== Building ==="
cmake --build "$BUILD_DIR" -j "$JOBS"

# ── Test ────────────────────────────────────────────────────────────────
echo "=== Testing ==="
cd "$BUILD_DIR" && ctest --output-on-failure ${CTEST_ARGS:-}

echo "=== Done ==="
