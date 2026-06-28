#!/usr/bin/env bash
# run_bench.sh - Run moo end-to-end benchmarks
#
# Usage:
#   ./bench/run_bench.sh [tcp|http|all]
#
# For micro-benchmarks, use: ./scripts/run_micro_bench.sh
#
# Prerequisites:
#   - Build with: cmake -B build -DX_BUILD_BENCHMARKS=ON && cmake --build build
#   - For HTTP benchmarks: install wrk (https://github.com/wg/wrk)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/../../build}"
RESULTS_DIR="${RESULTS_DIR:-${SCRIPT_DIR}/../bench_results}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

mkdir -p "$RESULTS_DIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# ── TCP echo benchmark ──

run_tcp() {
  local port="${TCP_PORT:-9000}"
  local msg_size="${TCP_MSG_SIZE:-128}"
  local num_msgs="${TCP_NUM_MSGS:-100000}"
  local concurrency="${TCP_CONCURRENCY:-4}"

  local server_bin="$BUILD_DIR/libx/bench/tcp_echo_server"
  local client_bin="$BUILD_DIR/libx/bench/tcp_echo_client"

  if [ ! -x "$server_bin" ] || [ ! -x "$client_bin" ]; then
    error "TCP echo binaries not found. Build with -DX_BUILD_BENCHMARKS=ON"
    return 1
  fi

  info "Starting TCP echo server on port $port..."
  "$server_bin" "$port" &
  local server_pid=$!
  sleep 1

  # Verify server is running
  if ! kill -0 "$server_pid" 2>/dev/null; then
    error "TCP echo server failed to start"
    return 1
  fi

  local out="$RESULTS_DIR/tcp_echo_${TIMESTAMP}.txt"
  info "Running TCP echo client (msg_size=$msg_size, msgs=$num_msgs, concurrency=$concurrency)"
  "$client_bin" 127.0.0.1 "$port" "$msg_size" "$num_msgs" "$concurrency" | tee "$out"

  info "Stopping TCP echo server..."
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  info "TCP echo benchmark completed → $out"
}

# ── HTTP benchmark ──

run_http() {
  local port="${HTTP_PORT:-8080}"
  local duration="${HTTP_DURATION:-10s}"
  local threads="${HTTP_THREADS:-4}"
  local connections="${HTTP_CONNECTIONS:-100}"

  local server_bin="$BUILD_DIR/libx/bench/http_bench_server"

  if [ ! -x "$server_bin" ]; then
    error "HTTP bench server not found. Build with -DX_BUILD_BENCHMARKS=ON and libcurl"
    return 1
  fi

  if ! command -v wrk &>/dev/null; then
    warn "wrk not found. Install wrk for HTTP benchmarks."
    warn "Trying with curl as a simple smoke test instead..."

    info "Starting HTTP bench server on port $port..."
    "$server_bin" "$port" &
    local server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
      error "HTTP bench server failed to start"
      return 1
    fi

    curl -s "http://127.0.0.1:$port/ping"
    echo

    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    return 0
  fi

  info "Starting HTTP bench server on port $port..."
  "$server_bin" "$port" &
  local server_pid=$!
  sleep 1

  if ! kill -0 "$server_pid" 2>/dev/null; then
    error "HTTP bench server failed to start"
    return 1
  fi

  local out="$RESULTS_DIR/http_bench_${TIMESTAMP}.txt"

  {
    echo "=== HTTP Benchmark Results ==="
    echo "Date: $(date)"
    echo "Duration: $duration, Threads: $threads, Connections: $connections"
    echo ""

    echo "--- GET /ping (minimal response) ---"
    wrk -t"$threads" -c"$connections" -d"$duration" \
        "http://127.0.0.1:$port/ping"
    echo ""

    echo "--- GET /echo?size=1024 (1KB response) ---"
    wrk -t"$threads" -c"$connections" -d"$duration" \
        "http://127.0.0.1:$port/echo?size=1024"
    echo ""

    echo "--- GET /echo?size=8192 (8KB response) ---"
    wrk -t"$threads" -c"$connections" -d"$duration" \
        "http://127.0.0.1:$port/echo?size=8192"
  } | tee "$out"

  info "Stopping HTTP bench server..."
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  info "HTTP benchmark completed → $out"
}

# ── DNS benchmark ──

run_dns() {
  local port="${DNS_PORT:-15353}"

  local server_bin="$BUILD_DIR/libx/bench/dns_bench_server"
  local xdns_bin="$BUILD_DIR/libx/bench/dns_bench_xdns"
  local xnet_bin="$BUILD_DIR/libx/bench/dns_bench_xnet"
  local cares_bin="$BUILD_DIR/libx/bench/dns_bench_cares"
  local go_bin="$SCRIPT_DIR/dns/dns_bench_client.go"

  if [ ! -x "$server_bin" ] || [ ! -x "$xdns_bin" ] || [ ! -x "$xnet_bin" ]; then
    error "DNS bench binaries not found. Build with -DX_BUILD_BENCHMARKS=ON"
    return 1
  fi

  # ── Local mode ──
  info "Starting local DNS bench server on port $port..."
  "$server_bin" "$port" &
  local server_pid=$!
  sleep 1

  if ! kill -0 "$server_pid" 2>/dev/null; then
    error "DNS bench server failed to start"
    return 1
  fi

  local out="$RESULTS_DIR/dns_bench_local_${TIMESTAMP}.json"

  # Each benchmark outputs one valid JSON array; merge with jq if available
  info "Running xdns benchmark (local)..."
  "$xdns_bin" local > "${out}.xdns"
  info "Running xnet/dns benchmark (local)..."
  "$xnet_bin" local > "${out}.xnet"
  if [ -x "$cares_bin" ]; then
    info "Running c-ares benchmark (local)..."
    "$cares_bin" local > "${out}.cares"
  fi
  if command -v go &>/dev/null && [ -f "$go_bin" ]; then
    info "Running Go DNS benchmark (local)..."
    GODEBUG=netdns=go go run "$go_bin" local > "${out}.go"
  else
    warn "Go not available — skipping Go benchmark"
  fi

  # Merge into single JSON array
  if command -v jq &>/dev/null; then
    jq -s 'add' "${out}".xdns "${out}".xnet "${out}".go "${out}".cares 2>/dev/null > "$out" || true
  else
    # Fallback: strip brackets, join with commas, wrap
    printf '[\n' > "$out"
    local first=1
    for f in "${out}".xdns "${out}".xnet "${out}".go "${out}".cares; do
      [ -f "$f" ] || continue
      [ "$first" -eq 1 ] || printf ',\n' >> "$out"
      first=0
      sed '1d;$d' "$f" >> "$out"
    done
    printf '\n]\n' >> "$out"
  fi

  info "Stopping local DNS bench server..."
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true

  # ── Remote mode ──
  # Flush system DNS cache for fair comparison (xdns doesn't use it)
  if [ "$(uname -s)" = "Darwin" ]; then
    if sudo -n true 2>/dev/null; then
      info "Flushing macOS DNS cache..."
      sudo dscacheutil -flushcache 2>/dev/null || true
      sudo killall -HUP mDNSResponder 2>/dev/null || true
    else
      warn "sudo not available — system DNS cache may skew results"
    fi
  elif [ "$(uname -s)" = "Linux" ]; then
    if sudo -n true 2>/dev/null; then
      info "Flushing Linux DNS cache..."
      sudo resolvectl flush-caches 2>/dev/null || \
      sudo systemd-resolve --flush-caches 2>/dev/null || true
    else
      warn "sudo not available — system DNS cache may skew results"
    fi
  fi

  # ── Remote mode ──
  out="$RESULTS_DIR/dns_bench_remote_${TIMESTAMP}.json"

  info "Running xdns benchmark (remote)..."
  "$xdns_bin" remote > "${out}.xdns"
  info "Running xnet/dns benchmark (remote)..."
  "$xnet_bin" remote > "${out}.xnet"
  if [ -x "$cares_bin" ]; then
    info "Running c-ares benchmark (remote)..."
    "$cares_bin" remote > "${out}.cares"
  fi
  if command -v go &>/dev/null && [ -f "$go_bin" ]; then
    info "Running Go DNS benchmark (remote)..."
    GODEBUG=netdns=go go run "$go_bin" remote > "${out}.go"
  fi

  # Merge
  if command -v jq &>/dev/null; then
    jq -s 'add' "${out}".xdns "${out}".xnet "${out}".go "${out}".cares 2>/dev/null > "$out" || true
  else
    printf '[\n' > "$out"
    local first=1
    for f in "${out}".xdns "${out}".xnet "${out}".go "${out}".cares; do
      [ -f "$f" ] || continue
      [ "$first" -eq 1 ] || printf ',\n' >> "$out"
      first=0
      sed '1d;$d' "$f" >> "$out"
    done
    printf '\n]\n' >> "$out"
  fi

  info "DNS benchmark completed → $out"
}

# ── Main ──

MODE="${1:-all}"

case "$MODE" in
  tcp)
    run_tcp
    ;;
  http)
    run_http
    ;;
  dns)
    run_dns
    ;;
  all)
    run_tcp
    echo ""
    run_http
    ;;
  *)
    echo "Usage: $0 [tcp|http|dns|all]"
    exit 1
    ;;
esac

info "All requested benchmarks completed. Results in: $RESULTS_DIR"
