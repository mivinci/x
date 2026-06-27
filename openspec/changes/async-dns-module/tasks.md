## 1. xSocket UDP support

- [x] 1.1 Add `xSocketSendTo` to `socket.h` and `socket.c` — non-blocking `sendto()` wrapper
- [x] 1.2 Add `xSocketRecvFrom` to `socket.h` and `socket.c` — non-blocking `recvfrom()` wrapper
- [x] 1.3 Test: sendto/recvfrom round-trip with two UDP sockets (in socket_test.cpp)

## 2. DNS packet build/parse

- [x] 2.1 Create `libx/x/dns/dns.h` — public types (xDnsType, xDnsRecord, xDnsHeader, xDnsQuestion)
- [x] 2.2 Create `libx/x/dns/dns_packet.c` — `dns_build_query()` (header + question + EDNS0 OPT record with UDP payload size 4096, per RFC 6891)
- [x] 2.3 Create `libx/x/dns/dns_packet.c` — `dns_build_response()` (header + question + answers)
- [x] 2.4 Create `libx/x/dns/dns_packet.c` — `dns_parse()` (header + question + answers, with compression)
- [x] 2.5 Test: packet build→parse round-trip (A, AAAA, CNAME, with EDNS0 OPT record)
- [x] 2.6 Test: compression pointer parsing (RFC 1035 §4.1.4)
- [x] 2.7 Test: malformed packet rejection (truncated, bad label length, loop in compression)

## 3. DNS cache

- [x] 3.1 Create `libx/x/dns/dns_cache.c` — TTL-based cache using xMap
- [x] 3.2 Test: cache insert/lookup/expiry

## 4. DNS config

- [x] 4.1 Create `libx/x/dns/dns_config.c` — platform-specific nameserver discovery
- [x] 4.2 POSIX: parse `/etc/resolv.conf` for `nameserver` lines
- [x] 4.3 Windows: use `GetNetworkParams()` from `iphlpapi.h` to discover DNS servers
- [x] 4.4 Fallback: use "8.8.8.8" if discovery fails
- [x] 4.5 Test: resolv.conf parsing with various formats (POSIX)
- [ ] 4.6 Test: Windows config discovery (guarded by `#ifdef _WIN32`)

## 5. DNS client (resolver)

- [x] 5.1 Create `libx/x/dns/dns_client.c` — xDnsClientCreate/Destroy, UDP socket setup
- [x] 5.2 Implement `xDnsClientDo` — cache check, for each bit in xDnsType: build query, sendto, register in query table, start timer; track pending count for multi-type merge
- [x] 5.3 Implement UDP readable callback — recvfrom, parse, match ID, invoke callback, cache result
- [x] 5.4 Implement timeout/retry — try next nameserver, or callback with error
- [x] 5.5 Test: resolve A record via real 8.8.8.8 (guarded by network reachability)
- [x] 5.6 Test: resolve AAAA record via real DNS (covered by A|AAAA test)
- [x] 5.7 Test: resolve A|AAAA together — verify both record types in result
- [x] 5.8 Test: partial success (A resolves, AAAA times out → only A records returned)
- [x] 5.9 Test: cache hit returns immediately
- [x] 5.10 Test: timeout with unreachable nameserver

## 6. DNS server

- [x] 6.1 Create `libx/x/dns/dns_server.c` — xDnsServerCreate/Destroy, UDP listener setup
- [x] 6.2 Implement `xDnsServerListen` — bind UDP socket, register with event loop
- [x] 6.3 Implement query handler — recvfrom, parse query, check zones, forward or NXDOMAIN
- [x] 6.4 Implement `xDnsZone` — create/destroy/add records, lookup by name+type
- [x] 6.5 Implement filter callback — call filter before zone/forward processing
- [x] 6.6 Implement forwarding — use xDnsClientDo, forward response back to client
- [x] 6.7 Test: authoritative zone query (local server + local client)
- [x] 6.8 Test: forwarding (local server → 8.8.8.8 → local client)
- [x] 6.9 Test: filter blocks query
- [x] 6.10 Test: NXDOMAIN for unknown name without forwarder

## 7. Build integration

- [x] 7.1 Create `libx/x/dns/CMakeLists.txt`
- [x] 7.2 Add `add_subdirectory(x/dns)` to `libx/CMakeLists.txt`
- [x] 7.3 Add `xdns` to aggregated `x` target in `libx/CMakeLists.txt`
- [x] 7.4 Windows: link `iphlpapi` for `GetNetworkParams()` (`if(WIN32) target_link_libraries(xdns PRIVATE iphlpapi)`)
- [x] 7.5 Verify build on macOS and Linux

## 8. Documentation

- [x] 8.1 Create `docs/libx/dns/README.md` — module overview
- [x] 8.2 Document xDnsClient API with examples
- [x] 8.3 Document xDnsServer API with examples (forwarding, authoritative, filter)
