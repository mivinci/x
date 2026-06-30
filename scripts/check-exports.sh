#!/usr/bin/env bash
# check-exports.sh — Verify that a shared library exports only allowlisted symbols.
#
# Usage:
#   scripts/check-exports.sh <library.so|.dylib> <allowlist.txt>
#
# Exits 0 if all exported symbols are in the allowlist, 1 otherwise.
# "Exported" = symbols with uppercase T in `nm` output (public text segment).
#
# See: openspec/changes/establish-abi-export-conventions/

set -euo pipefail

if [ $# -ne 2 ]; then
  echo "Usage: $0 <library> <allowlist>" >&2
  exit 1
fi

LIB="$1"
ALLOWLIST="$2"

if [ ! -f "$LIB" ]; then
  echo "error: library not found: $LIB" >&2
  exit 1
fi

if [ ! -f "$ALLOWLIST" ]; then
  echo "error: allowlist not found: $ALLOWLIST" >&2
  exit 1
fi

# Extract exported symbols (uppercase T = public text segment).
# On macOS, nm prepends '_' to C symbols; strip it for comparison.
# Filter to only our symbols (x*, dlp*, hls*) — external libraries
# (libcurl, OpenSSL, nghttp2, etc.) may re-export their symbols through
# our .dylib, but those are not our responsibility.
EXPORTED=$(nm "$LIB" 2>/dev/null \
  | awk '$2 == "T" { print $3 }' \
  | sed 's/^_//' \
  | grep -E '^(x|dlp|hls)')

# Check each exported symbol against the allowlist.
LEAKS=0
while IFS= read -r sym; do
  if ! grep -qx "$sym" "$ALLOWLIST"; then
    echo "LEAK: $sym (exported but not in allowlist)" >&2
    LEAKS=$((LEAKS + 1))
  fi
done <<< "$EXPORTED"

if [ "$LEAKS" -gt 0 ]; then
  echo "FAIL: $LEAKS unexpected exported symbol(s) in $LIB" >&2
  exit 1
fi

TOTAL=$(echo "$EXPORTED" | grep -c . || true)
ALLOWED=$(grep -c . "$ALLOWLIST" || true)
echo "OK: $LIB — $TOTAL exported symbols, all in allowlist ($ALLOWED entries)"
