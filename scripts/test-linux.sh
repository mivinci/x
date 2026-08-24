#!/usr/bin/env bash
# test-linux.sh — Build and test libx on Linux
#
# Usage:
#   scripts/test-linux.sh -t <openssl|mbedtls> [-j <jobs>] [-B <build-dir>] (--asan|--tsan)

set -euo pipefail

TLS_BACKEND="openssl"
JOBS=$(nproc 2>/dev/null || echo 4)
BUILD_DIR="build"
ASAN=0
TSAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -t) TLS_BACKEND="$2"; shift 2 ;;
    -j) JOBS="$2"; shift 2 ;;
    -B) BUILD_DIR="$2"; shift 2 ;;
    --asan) ASAN=1; shift ;;
    --tsan) TSAN=1; shift ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

if [[ $ASAN -eq 1 && $TSAN -eq 1 ]]; then
  echo "error: --asan and --tsan are mutually exclusive (incompatible runtimes); use separate build dirs"
  exit 1
fi

# ── Sanitizer configuration ─────────────────────────────────────────────
if [[ $ASAN -eq 1 ]]; then
  export ASAN_OPTIONS="halt_on_error=0:allocator_may_return_null=1:new_delete_type_mismatch=0"
  LSAN_SUPPRESSIONS="$(cd "$(dirname "$0")" && pwd)/lsan_suppressions.txt"
  if [[ -f "$LSAN_SUPPRESSIONS" ]]; then
    export LSAN_OPTIONS="suppressions=$LSAN_SUPPRESSIONS"
  fi
  CMAKE_SAN_FLAGS="-DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer' -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'"
elif [[ $TSAN -eq 1 ]]; then
  # TSan catches data races ASan cannot see (order/protocol races in the
  # sync primitives); the two runtimes are incompatible, hence the
  # separate --tsan lane. halt_on_error=1: fail the first race, clean CI signal.
  export TSAN_OPTIONS="halt_on_error=1:allocator_may_return_null=1"
  CMAKE_SAN_FLAGS="-DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -g' -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer'"
else
  CMAKE_SAN_FLAGS=""
fi

# ── Build ───────────────────────────────────────────────────────────────
echo "=== Configuring (TLS: $TLS_BACKEND, jobs: $JOBS, C++${CMAKE_CXX_STANDARD:-20}) ==="
eval cmake -B "$BUILD_DIR" -DX_TLS_BACKEND="$TLS_BACKEND" -DCMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD:-20} $CMAKE_SAN_FLAGS ${CMAKE_EXTRA_FLAGS:-}

echo "=== Building ==="
cmake --build "$BUILD_DIR" -j "$JOBS"

# ── Test ────────────────────────────────────────────────────────────────
echo "=== Testing ==="
cd "$BUILD_DIR" && ctest --output-on-failure ${CTEST_ARGS:-}

echo "=== Done ==="
