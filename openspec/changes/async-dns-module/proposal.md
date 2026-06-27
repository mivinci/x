## Why

The current DNS module (`libx/x/net/dns.c`) uses `getaddrinfo()` offloaded to a thread pool — "fake async" that blocks worker threads. A truly async DNS module implementing the DNS protocol directly over UDP eliminates thread pool dependency, enables DNS caching, and supports both client (resolver) and server (forwarding/authoritative) use cases.

## What Changes

- New module `libx/x/dns/` with client + server, depending only on `xbase`
- `xDnsClient`: async DNS resolver over UDP (no `getaddrinfo`, no thread pool)
  - Query A/AAAA/CNAME records
  - TTL-based cache (shared with server)
  - Timeout + retry with next nameserver
  - `/etc/resolv.conf` parsing (nameserver lines)
- `xDnsServer`: DNS server supporting forwarding and authoritative modes
  - UDP listener on port 53 (or custom)
  - Forwarding: forward unresolved queries to `xDnsClient` (upstream)
  - Authoritative: serve records from `xDnsZone`
  - Filter callback: intercept/rewrite/block queries
  - Shared cache with client
- `xDnsZone`: programmatic zone record management
- DNS packet builder/parser (shared by client and server)
  - Build query/response packets
  - Parse query/response packets
  - Handle DNS name compression
- Add `xSocketSendTo` / `xSocketRecvFrom` to `xbase/socket.h` for UDP support
- Existing `libx/x/net/dns.c` remains unchanged (parallel module, not a replacement)

## Capabilities

### New Capabilities
- `async-dns-client`: Truly async DNS resolver over UDP with caching, timeout/retry, and resolv.conf support
- `async-dns-server`: DNS server with forwarding, authoritative zones, and filter callback

### Modified Capabilities
- `xbase-socket-udp`: Add `xSocketSendTo` / `xSocketRecvFrom` for UDP datagram I/O

## Impact

- **New code**: `libx/x/dns/` (~2000 lines: packet, client, server, cache, config, tests)
- **Modified code**: `libx/x/base/socket.h` + `socket.c` (add sendto/recvfrom, ~50 lines)
- **Build**: New `libx/x/dns/CMakeLists.txt`, update `libx/CMakeLists.txt` to add subdirectory
- **Dependencies**: `xdns` depends on `xbase` only (xSocket, xTimer, xMap, xBuffer)
- **Tests**: Unit tests for packet round-trip, cache, config; integration tests with real 8.8.8.8 and local server
