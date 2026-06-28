## Context

Three DNS resolver implementations to compare:
1. `xdns` (`libx/x/dns/`) — protocol-native UDP, event loop driven
2. `xnet/dns` (`libx/x/net/dns.c`) — thread-pool + `getaddrinfo`
3. Go — `net.LookupHost` with `GODEBUG=netdns=go` (pure Go resolver)

Both local (zero-latency) and remote (8.8.8.8) benchmarks are desired to isolate resolver overhead from network effects.

## Goals / Non-Goals

**Goals:**
- Measure single-query latency for each resolver
- Measure batch throughput (N concurrent queries)
- Measure cache hit latency (xdns TTL cache only)
- Output JSON for easy comparison and charting
- Integrate into existing `run_bench.sh`

**Non-Goals:**
- Continuous benchmarking in CI
- Memory profiling
- CNAME chain resolution testing

## Decisions

### 1. Structure: standalone executables, not Google Benchmark micro-benchmarks

**Rationale**: Follows existing `libx/bench/` pattern. Each benchmark is a standalone executable, not a Google Benchmark target. This keeps Go integration clean (separate binary) and matches the existing HTTP/TCP benchmarks.

### 2. Local DNS server written in C++ using xdns server

A local `xDnsServer` on `127.0.0.1:5353` responds to `A` queries with a fixed IP. This eliminates network jitter for the local-mode comparison.

```c
// For hostnames matching pattern "bench-N.local" → 192.168.0.N
// Responses are instant (no I/O beyond localhost UDP)
```

### 3. Output format: JSON

```json
{
  "resolver": "xdns",
  "mode": "local",
  "results": [
    {"scenario": "single_a", "queries": 1, "latency_us": 234},
    {"scenario": "batch_100", "queries": 100, "latency_us": 5600},
    {"scenario": "cache_hit", "queries": 1, "latency_us": 2}
  ]
}
```

### 4. Go benchmark as standalone binary

`libx/bench/dns/dns_bench_client.go` uses `GODEBUG=netdns=go` to force the pure-Go resolver. It resolves the same hostnames against the same local/remote DNS server and outputs the same JSON format.

### 5. Remote benchmark uses 8.8.8.8

Real-world comparison. Each resolver queries Google's public DNS. Network jitter affects all three equally (run sequentially on same machine).

## Risks / Trade-offs

- **Go resolver may not support localhost:5353 by default**: Go's `net.LookupHost` uses the system resolver configured in `/etc/resolv.conf`. To point at `127.0.0.1:5353`, we either configure the system resolver (risky) or use a custom `net.Resolver` with `Dial` override.
- **`getaddrinfo` may cache**: The system's nscd or resolver library may cache results, giving `xnet/dns` an unfair advantage in repeated tests. Mitigation: use unique hostnames per query.
