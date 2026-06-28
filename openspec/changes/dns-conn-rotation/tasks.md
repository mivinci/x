## 1. Data structures

- [ ] 1.1 Add `struct xDnsConn_` to `dns_private.h` (sock, addr, addrlen, query_count, max_queries)
- [ ] 1.2 Add `udp_max_queries` field to `xDnsClientConf` in `dns.h`
- [ ] 1.3 Add conn array to `struct xDnsClient_` in `dns_private.h`

## 2. Connection lifecycle

- [ ] 2.1 Initialize per-nameserver connections in `client_create()` (open socket, register Read event)
- [ ] 2.2 Implement `conn_rotate()` — close old socket, open new, reset counter
- [ ] 2.3 Call `conn_rotate()` before send when `query_count >= max_queries`
- [ ] 2.4 Clean up all connections in `client_destroy()`

## 3. Send/recv routing

- [ ] 3.1 Route `send_query()` through correct connection based on ns_index
- [ ] 3.2 Route `on_readable()` through correct connection based on socket fd

## 4. Tests

- [ ] 4.1 Test per-connection query routing (queries to different ns go through correct sockets)
- [ ] 4.2 Test connection rotation triggers at `udp_max_queries` limit
- [ ] 4.3 Test rotation produces new socket fd
- [ ] 4.4 Test default (udp_max_queries=0) preserves original behavior
- [ ] 4.5 Run existing DNS tests, verify no regressions

## 5. Benchmarks

- [ ] 5.1 Run local DNS benchmark with and without rotation
- [ ] 5.2 Run remote DNS benchmark with and without rotation
- [ ] 5.3 Compare batch throughput improvement

## 6. Build and verify

- [ ] 6.1 Build with cmake, fix compilation errors
- [ ] 6.2 Run xdns_test, verify all tests pass
- [ ] 6.3 Run full test suite, verify no regressions
