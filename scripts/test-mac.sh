#!/usr/bin/env zsh
# test-mac.sh — Build and test libx on macOS
#
# Usage:
#   scripts/test-mac.sh -t <openssl|mbedtls> [-j <jobs>] [-B <build-dir>] [--asan]

set -euo pipefail

TLS_BACKEND="openssl"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
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

# ── Homebrew dependency detection ───────────────────────────────────────
OPENSSL_ROOT=$(brew --prefix openssl 2>/dev/null || echo "")
MBEDTLS_ROOT=$(brew --prefix mbedtls 2>/dev/null || echo "")

CMAKE_EXTRA_FLAGS=""
if [[ -n "$OPENSSL_ROOT" ]]; then
  CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DOPENSSL_ROOT_DIR=$OPENSSL_ROOT"
fi
if [[ -n "$MBEDTLS_ROOT" ]]; then
  CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DMbedTLS_DIR=$MBEDTLS_ROOT"
fi

# ── ASan configuration ──────────────────────────────────────────────────
if [[ $ASAN -eq 1 ]]; then
  export ASAN_OPTIONS="halt_on_error=0"
  LSAN_SUPPRESSIONS="$(cd "$(dirname "$0")" && pwd)/lsan_suppressions.txt"
  if [[ -f "$LSAN_SUPPRESSIONS" ]]; then
    export LSAN_OPTIONS="suppressions=$LSAN_SUPPRESSIONS"
  fi
  CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer' -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'"
fi

# ── Build ───────────────────────────────────────────────────────────────
echo "=== Configuring (TLS: $TLS_BACKEND, jobs: $JOBS) ==="
cmake -B "$BUILD_DIR" -DX_TLS_BACKEND="$TLS_BACKEND" $CMAKE_EXTRA_FLAGS

echo "=== Building ==="
cmake --build "$BUILD_DIR" -j "$JOBS"

# ── Test ────────────────────────────────────────────────────────────────
echo "=== Testing ==="
cd "$BUILD_DIR" && ctest --output-on-failure

echo "=== Done ==="
