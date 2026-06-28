#!/usr/bin/env bash
# flush_dns_cache.sh — Flush system DNS cache for fair benchmark comparison
#
# Usage: sudo bash flush_dns_cache.sh
#
# xdns uses its own protocol-native resolver (no system cache).
# c-ares / xnet / Go may be skewed by cached lookups.
# Run this before remote DNS benchmarks to level the field.
set -e

OS="$(uname -s)"

if [ "$(id -u)" != "0" ]; then
  echo "This script needs root to flush the system DNS cache."
  echo "Run: sudo bash $0"
  exit 1
fi

case "$OS" in
  Darwin)
    echo "[macOS] Flushing DNS cache..."
    dscacheutil -flushcache
    killall -HUP mDNSResponder 2>/dev/null || true
    echo "Done."
    ;;
  Linux)
    echo "[Linux] Flushing DNS cache..."
    if command -v resolvectl &>/dev/null; then
      resolvectl flush-caches || true
    elif command -v systemd-resolve &>/dev/null; then
      systemd-resolve --flush-caches || true
    else
      # Fallback: restart nscd
      if systemctl is-active nscd &>/dev/null 2>&1; then
        systemctl restart nscd || true
      fi
      # systemd-resolved restart
      if systemctl is-active systemd-resolved &>/dev/null 2>&1; then
        systemctl restart systemd-resolved || true
      fi
    fi
    echo "Done."
    ;;
  *)
    echo "Unknown OS: $OS — skipping DNS cache flush."
    exit 1
    ;;
esac
