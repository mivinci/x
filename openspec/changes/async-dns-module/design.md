## Context

DNS resolution in libx currently uses `getaddrinfo()` offloaded to a thread pool (`libx/x/net/dns.c`). This blocks worker threads and provides no caching, no control over the resolution process, and no server capability. A new `libx/x/dns/` module implements the DNS protocol directly over UDP, providing true async resolution plus a DNS server.

## Goals / Non-Goals

**Goals:**
- Truly async DNS client — no `getaddrinfo`, no thread pool, fully event-loop-driven
- DNS server with forwarding + authoritative modes
- Shared packet build/parse code between client and server
- TTL-based cache shared between client and server
- `/etc/resolv.conf` parsing for nameserver discovery
- Add `xSocketSendTo` / `xSocketRecvFrom` to xbase for UDP I/O
- Real DNS integration tests (query 8.8.8.8)
- Local server tests (server as mock for client)
- Follow relevant RFCs (see Standards Compliance section)

**Non-Goals:**
- TCP fallback for truncated responses (V2)
- DNSSEC validation (V3)
- DNS-over-HTTPS (V3)
- Recursive resolution (root → TLD → authoritative) — forwarding only
- MX/SRV/TXT/PTR record types (V2 — V1 supports A/AAAA/CNAME)
- mDNS / DNS-SD (V3)
- `/etc/hosts` file parsing (V2)
- Replacing `libx/x/net/dns.c` (parallel module)

## Standards Compliance

V1 implementation SHALL follow these RFCs:

| RFC | Title | Scope |
|-----|-------|-------|
| RFC 1034 | Domain Names — Concepts and Facilities | Caching behavior, CNAME resolution |
| RFC 1035 | Domain Names — Implementation and Specification | Packet format, name encoding, compression (§4.1.4), QTYPE/QCLASS values, TTL |
| RFC 3596 | DNS Extensions to Support IPv6 | AAAA record type (QTYPE=28, 16-byte RDATA) |
| RFC 6891 | Extension Mechanisms for DNS (EDNS(0)) | OPT pseudo-record in additional section; client advertises UDP payload size 4096 to avoid 512-byte truncation |

Key compliance requirements:
- **Name encoding** (RFC 1035 §4.1.4): labels as length-prefixed bytes, terminated by 0x00; compression pointers (`0xC0` prefix) supported in both build and parse
- **QCLASS=IN** (1) for all V1 queries
- **RD flag** set (recursion desired) in client queries
- **EDNS(0)** OPT record in additional section: version=0, UDP payload size=4096, no extended RCODEs
- **CNAME chains** (RFC 1034 §3.1): response may contain CNAME → A/AAAA records; parser returns all records in order
- **TTL** (RFC 1035 §3.2.1): cache stores TTL from response, entries expire at `now + TTL`
## Decisions

### D1: Module placement — `libx/x/dns/`

New top-level module, not under `libx/x/net/`. Depends on `xbase` only. This keeps the dependency graph clean: `xdns` doesn't need `xnet` (which currently has the thread-pool-based DNS).

```
xbase ←── xdns (new)
  ├── xSocket (UDP I/O)
  ├── xTimer (timeout/retry)
  ├── xMap (cache)
  └── xBuffer (packet buffer)

xbase ←── xnet (existing)
  └── dns.c (getaddrinfo thread pool — unchanged)
```

### D2: Shared packet code

DNS packet format is identical for client and server. A single `dns_packet.c` handles both:

```c
// Build
int dns_build_query(uint8_t *buf, size_t buflen, uint16_t id,
                    const char *name, xDnsType type);
int dns_build_response(uint8_t *buf, size_t buflen, uint16_t id, int rcode,
                       const xDnsRecord *answers);

// Parse
int dns_parse(const uint8_t *buf, size_t len, xDnsHeader *hdr,
              xDnsQuestion *q, xDnsRecord **answers);
```

DNS name compression (RFC 1035 §4.1.4) is handled in the parser — pointer references (`\xc0\x0c`) are followed to reconstruct full names.

### D3: Client architecture — single UDP socket, ID multiplexing

```
xDnsClient
  ├── 1 UDP socket (non-blocking, registered with event loop)
  ├── Query table: transaction ID → {name, callback, arg, timer, retries}
  ├── Cache: hostname+type → {records, expiry_time}
  └── Nameserver list: ["8.8.8.8", "1.1.1.1", ...]

Resolve flow:
  1. Check cache → hit? callback immediately
  2. Miss → assign 16-bit transaction ID
  3. Build query packet → sendto(nameserver:53)
  4. Start timeout timer (default 5s)
  5. Return (async)

UDP readable:
  1. recvfrom() → parse response
  2. Match transaction ID → find query in table
  3. Parse answer records → build xDnsRecord list
  4. Store in cache with TTL
  5. Cancel timer → invoke callback

Timeout:
  1. Cancel current query
  2. If retries remain → try next nameserver
  3. Else → callback with xErrno_Timeout
```

One socket handles all concurrent queries. Transaction ID (16-bit) multiplexes responses. ID collision probability is low for typical usage.

### D4: Server architecture — forwarding + authoritative

```
xDnsServer
  ├── 1 UDP socket (listener on port 53 or custom)
  ├── Zone records: name+type → {rdata, ttl}
  ├── Forwarder: xDnsClient (optional, NULL = authoritative only)
  ├── Filter callback (optional)
  ├── Cache (shared with forwarder if configured)
  └── Pending queries: client-addr+id → {forwarded-id, callback}

Query flow:
  1. recvfrom() → parse query
  2. Check zone records → hit? build response → sendto(client)
  3. Miss → if filter, call filter → block? sendto(NXDOMAIN)
  4. Pass → if forwarder, xDnsClientDo() → callback builds response → sendto(client)
  5. No forwarder → sendto(NXDOMAIN)
```

### D5: xSocketSendTo / xSocketRecvFrom

Add to `xbase/socket.h`:

```c
XCAPI(ssize_t) xSocketSendTo(xSocket sock, const void *buf, size_t len,
                              const struct sockaddr *dest, socklen_t destlen);
XCAPI(ssize_t) xSocketRecvFrom(xSocket sock, void *buf, size_t len,
                               struct sockaddr *src, socklen_t *srclen);
```

Implementation: non-blocking `sendto()` / `recvfrom()`, return bytes sent/received or -1 on error. No callback needed — caller uses `xSocketSetCallback` for readability notification, then calls `xSocketRecvFrom` in the callback.

### D6: Naming convention — consistent with HTTP module

```
HTTP:                    DNS:
xHttpClient              xDnsClient
xHttpClientDo            xDnsClientDo
xHttpServer              xDnsServer
xHttpServerListen        xDnsServerListen
xHttpServerCreate        xDnsServerCreate
```

### D7: xDnsType — bitmask query flags, not DNS QTYPE values

`xDnsType` is an API-level bitmask that callers can OR together. Internally, each bit maps to a DNS QTYPE and triggers a separate UDP query. Results are merged into a single `xDnsRecord` list before invoking the callback once.

```c
// API level — bitmask, can OR
typedef enum {
  xDnsType_A     = 1 << 0,   // maps to QTYPE=1
  xDnsType_AAAA  = 1 << 1,   // maps to QTYPE=28
  xDnsType_CNAME = 1 << 2,   // maps to QTYPE=5
} xDnsType;

// Record type in results — actual DNS QTYPE (individual, not bitmask)
XDEF_STRUCT(xDnsRecord) {
  uint16_t    qtype;      // 1=A, 28=AAAA, 5=CNAME
  uint32_t    ttl;
  const char *name;
  const void *rdata;
  size_t      rdlength;
  xDnsRecord *next;
};
```

Multi-type query flow:
```
xDnsClientDo("example.com", xDnsType_A | xDnsType_AAAA, cb, arg)
  ├── Send QTYPE=1 query  → ID=1234
  ├── Send QTYPE=28 query → ID=1235
  ├── Wait for all to complete (or timeout)
  ├── ID=1234 response → parse A records → accumulate
  ├── ID=1235 response → parse AAAA records → accumulate
  └── All done → merge into one xDnsRecord list → invoke cb once

  Partial success: A resolves, AAAA times out → cb with A records only
  All timeout → cb with xErrno_Timeout
```

DNS QTYPE values (1, 28, 5) are NOT powers of 2, so they can't be used directly as bitmask flags. The API type (`xDnsType`) is separate from the wire type (`uint16_t qtype` in `xDnsRecord`).

### D8: V1 record types — A, AAAA, CNAME only

A (IPv4) and AAAA (IPv6) are the primary use case. CNAME is needed because many domains return CNAME chains before the final A/AAAA record. Other types (MX, SRV, TXT, PTR) deferred to V2.

### D8: Testing — real DNS + local server

- **Unit tests**: packet build→parse round-trip, cache TTL, config parsing
- **Integration tests (client)**: query `8.8.8.8` for `google.com` A record, verify valid IP returned
- **Integration tests (server)**: start local server with zone, query it with local client, verify response
- **Integration tests (forwarding)**: local server forwards to `8.8.8.8`, client queries local server

Real DNS tests may fail on offline CI — guard with `if (can_reach("8.8.8.8"))` or skip on network error.

### D9: Platform-specific DNS config discovery

DNS nameserver discovery differs by platform:

| Platform | Source | Method |
|----------|--------|--------|
| Linux / macOS | `/etc/resolv.conf` | Read file, parse `nameserver` lines |
| Windows | System network config | `GetNetworkParams()` API (`iphlpapi.h`) |
| Fallback | Hardcoded | `8.8.8.8` |

`dns_config.c` uses `#ifdef _WIN32` to select the implementation:

```c
// POSIX
static int load_nameservers(char buf[][46], int max) {
  FILE *f = fopen("/etc/resolv.conf", "r");
  // parse "nameserver 8.8.8.8" lines
}

// Windows
static int load_nameservers(char buf[][46], int max) {
  ULONG len = 0;
  GetNetworkParams(NULL, &len);  // get required size
  FIXED_INFO *info = malloc(len);
  GetNetworkParams(info, &len);
  // walk info->DnsServerList linked list
  // link ipaddr_string_to_str(&info->DnsServerList, buf[0], 46);
}
```

Windows build requires linking `iphlpapi.lib` (added in CMakeLists.txt via `target_link_libraries(xdns PRIVATE iphlpapi)` on `WIN32`).

## Risks / Trade-offs

- **[DNS compression bugs]** → DNS name compression (pointer references) is the trickiest part of parsing. Mitigate with thorough round-trip tests.
- **[Transaction ID collision]** → 16-bit ID, 65536 values. For typical usage (a few concurrent queries) collision probability is negligible. If needed, V2 can use source port randomization.
- **[No TCP fallback]** → UDP responses > 512 bytes are truncated (TC flag). V1 ignores TC and returns what was received. V2 adds TCP retry.
- **[Real DNS test flakiness]** → Tests against 8.8.8.8 may fail on offline CI or rate-limited networks. Guard with network reachability check.
- **[Platform config differences]** → `/etc/resolv.conf` (POSIX) vs `GetNetworkParams()` (Windows). Both are well-documented and stable APIs. Fallback to 8.8.8.8 if discovery fails.
- **[Parallel module]** → `libx/x/net/dns.c` remains. Users can choose old (thread pool) or new (async). No forced migration.
