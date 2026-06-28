# dns.h — DNS Client API

## Introduction

`xDnsClient` provides truly asynchronous DNS resolution over UDP. It implements the DNS protocol directly (RFC 1035 + RFC 6891 EDNS0), with no `getaddrinfo` and no thread pool. All I/O is non-blocking and driven by the event loop.

## Types

### xDnsClientConf — Configuration

```c
XDEF_STRUCT(xDnsClientConf) {
  const char *nameservers[8];  // Up to 8 nameservers ("8.8.8.8", ...)
  int          timeout_ms;     // Per-query timeout (default 5000)
  int          retries;        // Retries with next nameserver (default 2)
  int          enable_cache;   // 1 = enable TTL cache (default 1)
};
```

Zero-initialize for defaults. When `nameservers[0]` is NULL, the client auto-discovers DNS servers from `/etc/resolv.conf` (POSIX) or `GetNetworkParams()` (Windows), falling back to `8.8.8.8`.

Nameserver strings support optional port: `"8.8.8.8:53"` or `"127.0.0.1:5353"`.

### xDnsCallback — Completion callback

```c
typedef void (*xDnsCallback)(xErrno err, const xDnsRecord *records, void *arg);
```

- `err` — `xErrno_Ok` on success (including partial success where some record types resolved and others timed out)
- `records` — Linked list of resolved records, valid only during the callback
- `arg` — User-provided argument from `xDnsClientDo()`

## API

```c
XCAPI(xDnsClient) xDnsClientCreate(const xDnsClientConf *conf);
XCAPI(void)        xDnsClientDestroy(xDnsClient client);
XCAPI(xErrno)      xDnsClientDo(xDnsClient client, const char *name,
                                xDnsType type, xDnsCallback cb, void *arg);
```

### Lifecycle

| Function | Description |
| --- | --- |
| `xDnsClientCreate(conf)` | Create a client bound to the current event loop. `conf` may be NULL for defaults. |
| `xDnsClientDestroy(client)` | Destroy the client. In-flight queries are cancelled; their callbacks are NOT invoked. Safe with NULL. |
| `xDnsClientDo(client, name, type, cb, arg)` | Resolve a hostname. `type` is a bitmask of `xDnsType_A`, `xDnsType_AAAA`, `xDnsType_CNAME`. `cb` must not be NULL. |

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
conf.timeout_ms = 5000;

xDnsClient client = xDnsClientCreate(&conf);
xDnsClientDo(client, "example.com", xDnsType_A, on_resolve, NULL);
```

### Query A + AAAA together

```c
// Send both A and AAAA queries in parallel, get merged results
xDnsClientDo(client, "google.com", xDnsType_A | xDnsType_AAAA,
             on_resolve, NULL);
```

### Auto-discover nameservers

```c
xDnsClientConf conf = {};  // nameservers[0] = NULL → auto-detect
xDnsClient client = xDnsClientCreate(&conf);
// Reads /etc/resolv.conf on Linux/macOS, GetNetworkParams() on Windows
// Falls back to 8.8.8.8
```

### With TTL caching

```c
xDnsClientConf conf = {};
conf.nameservers[0] = "8.8.8.8";
conf.enable_cache = 1;  // default

xDnsClient client = xDnsClientCreate(&conf);
// First query: sends UDP, caches result with TTL
xDnsClientDo(client, "example.com", xDnsType_A, cb1, NULL);
// Second query (within TTL): returns cached result immediately
xDnsClientDo(client, "example.com", xDnsType_A, cb2, NULL);
```

## How It Works

1. **Cache check** — If caching is enabled and a non-expired entry exists, the callback is invoked immediately without network I/O.
2. **Query** — For each bit in `xDnsType`, a DNS query packet is built (with EDNS0 OPT record, UDP payload size 4096) and sent via `xSocketSendTo` to the first nameserver.
3. **Wait** — Each query has a timeout timer. The event loop drives I/O — when the UDP socket becomes readable, the response is parsed and matched by transaction ID.
4. **Merge** — For multi-type queries, results are accumulated. The callback fires once when all queries complete (or time out).
5. **Retry** — On timeout, the client tries the next nameserver (up to `retries` times).
6. **Cache** — Successful results are stored in the TTL cache for future queries.

## Best Practices

- **Create one client per event loop** — A single client multiplexes all queries over one UDP socket.
- **Register before the event loop runs** — All queries must be initiated from the event loop thread.
- **Copy data in callbacks** — `xDnsRecord` pointers are valid only during the callback.
- **Handle partial success** — `xDnsType_A | xDnsType_AAAA` may succeed for A and time out for AAAA; check each record's `qtype`.
