#!/usr/bin/env bash
# run-clang-tidy.sh — Run clang-tidy on the project's test files.
#
# Reads the .clang-tidy config at the repo root and applies it to all
# *_test.cpp files under libx/ and libdlproxy/. Requires build/compile_commands.json
# (run `cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` first if missing).
#
# Exit code: 0 if no diagnostics, 1 otherwise. Used by CI.
#
# Usage:
#   scripts/run-clang-tidy.sh             # scan all test files
#   scripts/run-clang-tidy.sh <file>      # scan a single file
#   scripts/run-clang-tidy.sh -j 8        # parallel jobs (default: nproc)
#
# See:
#   .clang-tidy                  — config (which checks are enabled)
#   .github/workflows/ci.yml     — CI lane that calls this script

set -euo pipefail

# Locate the repo root so the script works regardless of CWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$ROOT/build"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# Allow override via env (CI sets these).
CT="${CLANG_TIDY:-clang-tidy}"
if ! command -v "$CT" >/dev/null 2>&1; then
  # Try Homebrew LLVM on macOS (not on PATH by default).
  if [ -x /opt/homebrew/opt/llvm/bin/clang-tidy ]; then
    CT=/opt/homebrew/opt/llvm/bin/clang-tidy
  elif [ -x /usr/local/opt/llvm/bin/clang-tidy ]; then
    CT=/usr/local/opt/llvm/bin/clang-tidy
  else
    echo "error: clang-tidy not found. Install via 'brew install llvm' or your package manager." >&2
    exit 2
  fi
fi

# Parse simple flags.
FILES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -j) JOBS="$2"; shift 2 ;;
    -B) BUILD="$2"; shift 2 ;;
    *)  FILES+=("$1"); shift ;;
  esac
done

# Default: scan all *_test.cpp files.
if [ ${#FILES[@]} -eq 0 ]; then
  while IFS= read -r line; do
    FILES+=("$line")
  done < <(find "$ROOT/libx" "$ROOT/libdlproxy" -name '*_test.cpp')
fi

if [ ! -f "$BUILD/compile_commands.json" ]; then
  echo "error: $BUILD/compile_commands.json not found." >&2
  echo "  Run: cmake -B $BUILD -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  exit 2
fi

echo "clang-tidy: $CT"
echo "build:      $BUILD"
echo "files:      ${#FILES[@]}"
echo "jobs:       $JOBS"
echo

# Run in parallel, capturing all output. clang-tidy exits non-zero if
# it finds errors with -Werror (set via WarningsAsErrors in .clang-tidy).
#
# On macOS with Homebrew LLVM, clang-tidy needs both LLVM's libc++ and
# the macOS SDK to parse test files (which include gtest, OpenSSL, etc.
# via system paths). The --extra-arg below makes the bundled clang
# driver look up the right headers.
EXTRA_ARGS=()
if [ -x /opt/homebrew/opt/llvm/bin/clang-tidy ] || [ -x /usr/local/opt/llvm/bin/clang-tidy ]; then
  EXTRA_ARGS+=(--extra-arg=-stdlib=libc++)
  # On macOS, point to the SDK so <cstdio>, <cmath>, <math.h> resolve.
  if command -v xcrun >/dev/null 2>&1; then
    SYSROOT="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [ -n "$SYSROOT" ]; then
      EXTRA_ARGS+=(--extra-arg=--sysroot="$SYSROOT")
    fi
  fi
fi

FAILED=0
printf '%s\n' "${FILES[@]}" | xargs -P "$JOBS" -I{} "$CT" -p "$BUILD" "${EXTRA_ARGS[@]}" {} 2>&1 \
  | tee /tmp/clang-tidy-output.txt || FAILED=1

# Always summarize, even on failure.
WARNINGS=$(grep -cE 'warning:' /tmp/clang-tidy-output.txt || true)
ERRORS=$(grep -cE 'error:' /tmp/clang-tidy-output.txt || true)
echo
echo "=== Summary ==="
echo "Warnings: $WARNINGS"
echo "Errors:   $ERRORS"
echo

if [ "$FAILED" -ne 0 ] || [ "$ERRORS" -gt 0 ]; then
  echo "FAIL: clang-tidy found violations"
  exit 1
fi

echo "OK: no clang-tidy violations"
exit 0
