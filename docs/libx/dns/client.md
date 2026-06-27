# xDnsClient — Async DNS Resolver

## Overview

`xDnsClient` provides truly asynchronous DNS resolution over UDP. It implements the DNS protocol directly (RFC 1035 + RFC 6891 EDNS0), with no `getaddrinfo` and no thread pool. All I/O is non-blocking and driven by the event loop.

## Types

### xDnsType — bitmask query flags

```c
typedef enum {
  xDnsType_A     = 1 << 0,   // IPv4 (QTYPE=1)
  xDnsType_AAAA  = 1 << 1,   // IPv6 (QTYPE=28)
  xDnsType_CNAME = 1 << 2,   // Canonical name (QTYPE=5)
} xDnsType;
```

Flags can be OR'd together: `xDnsType_A | xDnsType_AAAA` sends two parallel queries and merges results.

### xDnsRecord — resolved record

```c
XDEF_STRUCT(xDnsRecord) {
  uint16_t    qtype;      // DNS QTYPE: 1=A, 28=AAAA, 5=CNAME
  uint32_t    ttl;        // Time-to-live in seconds
  const char *name;       // Record name (NUL-terminated)
  const void *rdata;      // Record data (A: 4 bytes, AAAA: 16 bytes)
  size_t      rdlength;   // Length of rdata
  xDnsRecord *next;       // Next record in list, or NULL
};
```

### xDnsClientConf — configuration

```c
XDEF_STRUCT(xDnsClientConf) {
  const char *nameservers[8];  // "8.8.8.8", "1.1.1.1", ... (NULL = auto-detect)
  int          timeout_ms;     // Per-query timeout (default 5000)
  int          retries;        // Retry count per nameserver (default 2)
  int          enable_cache;   // 1 = enable TTL cache (default 1)
};
```

When `nameservers[0]` is NULL, the client auto-discovers DNS servers from `/etc/resolv.conf` (POSIX) or `GetNetworkParams()` (Windows), falling back to `8.8.8.8`.

Nameserver strings support optional port: `"8.8.8.8:53"` or `"127.0.0.1:5353"`.

## API

```c
XCAPI(xDnsClient) xDnsClientCreate(const xDnsClientConf *conf);
XCAPI(void)        xDnsClientDestroy(xDnsClient client);
XCAPI(xErrno)      xDnsClientDo(xDnsClient client, const char *name,
                                xDnsType type, xDnsCallback callback, void *arg);
```

### Callback

```c
typedef void (*xDnsCallback)(xErrno err, const xDnsRecord *records, void *arg);
```

- `err` is `xErrno_Ok` on success (including partial success where some record types resolved and others timed out)
- `records` is a linked list of resolved records, valid only during the callback
- The callback is invoked on the event loop thread

## Usage Examples

### Basic A record query

```c
void on_resolve(xErrno err, const xDnsRecord *records, void *arg) {
    if (err != xErrno_Ok) return;
    for (const xDnsRecord *r = records; r; r = r->next) {
        if (r->qtype == 1 /* A */) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, r->rdata, ip, sizeof(ip));
            printf("A: %s (TTL=%u)\n", ip, r->ttl);
        }
    }
}

xDnsClientConf conf = {};
conf.nameservers[0] = "8.8.8.8";
conf.timeout_ms     = 5000;

xDnsClient client = xDnsClientCreate(&conf);
xDnsClientDo(client, "example.com", xDnsType_A, on_resolve, NULL);

// Pump event loop...
xEventLoopRun(loop, X_RUN_DEFAULT);

xDnsClientDestroy(client);
```

### Query A + AAAA together

```c
// Send both A and AAAA queries in parallel, get merged results
xDnsClientDo(client, "google.com", xDnsType_A | xDnsType_AAAA, on_resolve, NULL);
```

### Auto-discover nameservers

```c
xDnsClientConf conf = {};  // nameservers[0] = NULL → auto-detect
xDnsClient client = xDnsClientCreate(&conf);
// Reads /etc/resolv.conf on Linux/macOS
// Uses GetNetworkParams() on Windows
// Falls back to 8.8.8.8
```

### With caching

```c
xDnsClientConf conf = {};
conf.nameservers[0] = "8.8.8.8";
conf.enable_cache   = 1;  // default

xDnsClient client = xDnsClientCreate(&conf);
// First query: sends UDP, caches result with TTL
xDnsClientDo(client, "example.com", xDnsType_A, cb1, NULL);
// Second query (within TTL): returns cached result immediately
xDnsClientDo(client, "example.com", xDnsType_A, cb2, NULL);
```

## How It Works

1. **Cache check**: If caching is enabled and a non-expired entry exists, the callback is invoked immediately without network I/O.
2. **Query**: For each bit in `xDnsType`, a DNS query packet is built (with EDNS0 OPT record, UDP payload size 4096) and sent via `xSocketSendTo` to the first nameserver.
3. **Wait**: Each query has a timeout timer. The event loop drives I/O — when the UDP socket becomes readable, the response is parsed and matched by transaction ID.
4. **Merge**: For multi-type queries, results are accumulated. The callback fires once when all queries complete (or timeout).
5. **Retry**: On timeout, the client tries the next nameserver (up to `retries` times).
6. **Cache**: Successful results are stored in the TTL cache for future queries.

## RFC Compliance

| RFC | Compliance |
|-----|-----------|
| RFC 1034 | Caching behavior, CNAME chains |
| RFC 1035 | Packet format, name encoding, compression pointers |
| RFC 3596 | AAAA record type (QTYPE=28) |
| RFC 6891 | EDNS0 OPT record, UDP payload size 4096 |
