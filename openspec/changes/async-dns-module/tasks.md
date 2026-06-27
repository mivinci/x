## 1. xSocket UDP support

- [ ] 1.1 Add `xSocketSendTo` to `socket.h` and `socket.c` — non-blocking `sendto()` wrapper
- [ ] 1.2 Add `xSocketRecvFrom` to `socket.h` and `socket.c` — non-blocking `recvfrom()` wrapper
- [ ] 1.3 Test: sendto/recvfrom round-trip with two UDP sockets

## 2. DNS packet build/parse

- [ ] 2.1 Create `libx/x/dns/dns.h` — public types (xDnsType, xDnsRecord, xDnsHeader, xDnsQuestion)
- [ ] 2.2 Create `libx/x/dns/dns_packet.c` — `dns_build_query()` (header + question + EDNS0 OPT record with UDP payload size 4096, per RFC 6891)
- [ ] 2.3 Create `libx/x/dns/dns_packet.c` — `dns_build_response()` (header + question + answers)
- [ ] 2.4 Create `libx/x/dns/dns_packet.c` — `dns_parse()` (header + question + answers, with compression)
- [ ] 2.5 Test: packet build→parse round-trip (A, AAAA, CNAME, with EDNS0 OPT record)
- [ ] 2.6 Test: compression pointer parsing (RFC 1035 §4.1.4)
- [ ] 2.7 Test: malformed packet rejection (truncated, bad label length, loop in compression)

## 3. DNS cache

- [ ] 3.1 Create `libx/x/dns/dns_cache.c` — TTL-based cache using xMap
- [ ] 3.2 Test: cache insert/lookup/expiry

## 4. DNS config

- [ ] 4.1 Create `libx/x/dns/dns_config.c` — platform-specific nameserver discovery
- [ ] 4.2 POSIX: parse `/etc/resolv.conf` for `nameserver` lines
- [ ] 4.3 Windows: use `GetNetworkParams()` from `iphlpapi.h` to discover DNS servers
- [ ] 4.4 Fallback: use "8.8.8.8" if discovery fails
- [ ] 4.5 Test: resolv.conf parsing with various formats (POSIX)
- [ ] 4.6 Test: Windows config discovery (guarded by `#ifdef _WIN32`)

## 5. DNS client (resolver)

- [ ] 5.1 Create `libx/x/dns/dns_client.c` — xDnsClientCreate/Destroy, UDP socket setup
- [ ] 5.2 Implement `xDnsClientDo` — cache check, for each bit in xDnsType: build query, sendto, register in query table, start timer; track pending count for multi-type merge
- [ ] 5.3 Implement UDP readable callback — recvfrom, parse, match ID, invoke callback, cache result
- [ ] 5.4 Implement timeout/retry — try next nameserver, or callback with error
- [ ] 5.5 Test: resolve A record via real 8.8.8.8 (guarded by network reachability)
- [ ] 5.6 Test: resolve AAAA record via real DNS
- [ ] 5.7 Test: resolve A|AAAA together — verify both record types in result
- [ ] 5.8 Test: partial success (A resolves, AAAA times out → only A records returned)
- [ ] 5.9 Test: cache hit returns immediately
- [ ] 5.10 Test: timeout with unreachable nameserver

## 6. DNS server

- [ ] 6.1 Create `libx/x/dns/dns_server.c` — xDnsServerCreate/Destroy, UDP listener setup
- [ ] 6.2 Implement `xDnsServerListen` — bind UDP socket, register with event loop
- [ ] 6.3 Implement query handler — recvfrom, parse query, check zones, forward or NXDOMAIN
- [ ] 6.4 Implement `xDnsZone` — create/destroy/add records, lookup by name+type
- [ ] 6.5 Implement filter callback — call filter before zone/forward processing
- [ ] 6.6 Implement forwarding — use xDnsClientDo, forward response back to client
- [ ] 6.7 Test: authoritative zone query (local server + local client)
- [ ] 6.8 Test: forwarding (local server → 8.8.8.8 → local client)
- [ ] 6.9 Test: filter blocks query
- [ ] 6.10 Test: NXDOMAIN for unknown name without forwarder

## 7. Build integration

- [ ] 7.1 Create `libx/x/dns/CMakeLists.txt`
- [ ] 7.2 Add `add_subdirectory(x/dns)` to `libx/CMakeLists.txt`
- [ ] 7.3 Add `xdns` to aggregated `x` target in `libx/CMakeLists.txt`
- [ ] 7.4 Windows: link `iphlpapi` for `GetNetworkParams()` (`if(WIN32) target_link_libraries(xdns PRIVATE iphlpapi)`)
- [ ] 7.5 Verify build on macOS and Linux

## 8. Documentation

- [ ] 8.1 Create `docs/libx/dns/README.md` — module overview
- [ ] 8.2 Document xDnsClient API with examples
- [ ] 8.3 Document xDnsServer API with examples (forwarding, authoritative, filter)
