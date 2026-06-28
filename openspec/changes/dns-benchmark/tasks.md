## 1. Local DNS benchmark server

- [x] 1.1 Create `libx/bench/dns/dns_bench_server.cpp` — xDnsServer on 127.0.0.1:15353, 100 zones
- [x] 1.2 Add `dns_bench_server` target to `libx/bench/CMakeLists.txt`

## 2. Libx benchmark clients (split due to header clash)

- [x] 2.1 Create `libx/bench/dns/dns_bench_xdns.cpp` and `dns_bench_xnet.cpp`
- [x] 2.2 Implement single-query latency test (A record) for both
- [x] 2.3 Implement batch throughput test (N concurrent queries) for both
- [x] 2.4 Implement cache hit test (xdns only)
- [x] 2.5 Run both local and remote modes
- [x] 2.6 Output JSON results to stdout for both
- [x] 2.7 Add `dns_bench_xdns` and `dns_bench_xnet` targets to CMakeLists.txt

## 3. Go benchmark client

- [x] 3.1 Create `libx/bench/dns/dns_bench_client.go` with `PreferGo: true`
- [x] 3.2 Implement same scenarios (single, batch, cache)
- [x] 3.3 Output same JSON format
- [x] 3.4 Use custom `net.Resolver.Dial` for local mode

## 4. Script integration

- [x] 4.1 Add `dns` subcommand to `libx/bench/run_bench.sh`
- [x] 4.2 Orchestrate: start local server → run libx clients → run Go client → stop server

## 5. Build and verify

- [x] 5.1 Build with `-DX_BUILD_BENCHMARKS=ON`, all compile
- [x] 5.2 Run local mode, JSON output well-formed; xdns wins dramatically (~2us cache hit)
- [ ] 5.3 Run remote mode (requires network, skip in CI)
