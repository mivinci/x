#!/bin/bash
# .container/run-ci.sh — Build image and run local Linux CI
#
# Usage:
#   .container/run-ci.sh                    # openssl, no ASAN (default)
#   .container/run-ci.sh mbedtls            # mbedTLS, no ASAN
#   .container/run-ci.sh openssl asan       # openssl + ASAN (needs more container memory)

set -e
cd "$(dirname "$0")/.."

TLS="${1:-openssl}"
ASAN_FLAG=""
[[ "${2:-}" == "asan" ]] && ASAN_FLAG="--asan"

IMAGE="libx-ci:latest"
BDIR="build-ci"

# Build image (cached on subsequent runs)
echo "=== Build container image ==="
container build -t "$IMAGE" -f .container/Containerfile .container

echo ""
echo "=== Run CI ($TLS, ${ASAN_FLAG:-(no asan)}) ==="
container run --rm -v "$PWD:/workspace" "$IMAGE" bash -c "
  rm -rf /workspace/$BDIR
  scripts/test-linux.sh -t '$TLS' -j 2 -B $BDIR $ASAN_FLAG
"
