## Why

xdns uses a single shared UDP socket for all queries to all nameservers. Under high concurrency, this causes:
- **QID exhaustion** — 16-bit transaction ID space shared across all queries
- **Kernel buffer pressure** — single UDP recv buffer fills with stale responses from timed-out queries
- **DNS server rate-limiting** — all queries appear from the same source port, triggering server-side throttling

c-ares solves this with per-server connections and `udp_max_queries` rotation: each nameserver gets its own UDP socket, and sockets are recycled after N queries to reset QID space and OS buffer state. Adding this pattern to xdns directly improves the concurrency bottleneck seen in benchmarks.

## What Changes

- Add per-nameserver UDP connection management (`xDnsConn`) to `dns_client.c`
- Add `udp_max_queries` config option (default 0 = unlimited, unchanged behavior)
- When `udp_max_queries > 0` and a connection exceeds the limit, close and reopen with a new source port
- Per-connection state: fd, query count, server reference
- Maintain a `xDnsConn → queries` mapping for response dispatch with existing QID table
- Backward-compatible: existing single-socket behavior when `udp_max_queries = 0`

## Capabilities

### New Capabilities

- `dns-conn-rotation`: Per-nameserver connection management with automatic rotation when `udp_max_queries` is exceeded.

### Modified Capabilities

<!-- None — existing API unchanged -->

## Impact

- `libx/x/dns/dns.h` — add `udp_max_queries` to `xDnsClientConf`
- `libx/x/dns/dns_private.h` — new `struct xDnsConn_`, add to `struct xDnsClient_`
- `libx/x/dns/dns_client.c` — connection lifecycle, rotation logic
- `libx/x/dns/dns_test.cpp` — test rotation behavior
- `libx/bench/dns/` — re-run benchmarks with connection rotation
