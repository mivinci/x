#!/bin/bash
# .container/run-ci.sh — Build image and run local Linux CI
#
# Usage:
#   .container/run-ci.sh                    # openssl, ASAN
#   .container/run-ci.sh mbedtls            # mbedTLS, ASAN
#   .container/run-ci.sh openssl no-asan    # openssl, no ASAN (faster)

set -e
cd "$(dirname "$0")/.."

TLS="${1:-openssl}"
ASAN="${2:-asan}"
IMAGE="libx-ci:latest"
BDIR="build-ci"

# Build image (cached on subsequent runs)
echo "=== Build container image ==="
container build -t "$IMAGE" -f .container/Containerfile .container > /dev/null 2>&1

echo ""
echo "=== Run CI ($TLS, $ASAN) ==="
container run --rm -v "$PWD:/workspace" "$IMAGE" bash -c "
  rm -rf /workspace/$BDIR
  if [ '$ASAN' = 'no-asan' ]; then
    scripts/test-linux.sh -t '$TLS' -j 2 -B $BDIR
  else
    scripts/test-linux.sh -t '$TLS' -j 2 -B $BDIR --asan
  fi
"
