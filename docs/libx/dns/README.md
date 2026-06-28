# dns.h — Async DNS Client + Server

## Introduction

`dns.h` is libx's DNS module, providing both a **client** (resolver) and **server** (authoritative/forwarding) built directly on the DNS protocol (RFC 1035, RFC 6891) over UDP. Unlike `libx/x/net/dns.c` which offloads `getaddrinfo()` to a thread pool, `dns.h` implements the protocol directly — truly async, no thread pool, no blocking calls.

All I/O is driven by xbase's event loop. The module depends on `xbase` only.

## Types

### xDnsType — Query type bitmask

```c
XDEF_ENUM(xDnsType){
  xDnsType_A     = 1 << 0,   // IPv4 address          → QTYPE=1
  xDnsType_AAAA  = 1 << 1,   // IPv6 address          → QTYPE=28
  xDnsType_CNAME = 1 << 2,   // Canonical alias       → QTYPE=5
};
```

Flags can be OR'd together: `xDnsType_A | xDnsType_AAAA` sends two parallel queries and merges results into a single callback.

### xDnsRecord — Resolved record

```c
XDEF_STRUCT(xDnsRecord) {
  uint16_t    qtype;      // DNS QTYPE (1=A, 28=AAAA, 5=CNAME)
  uint32_t    ttl;        // Time-to-live in seconds
  const char *name;       // Owner name (NUL-terminated, lowercase)
  const void *rdata;      // Raw RDATA (A: 4 bytes, AAAA: 16 bytes)
  size_t      rdlength;   // Length of rdata
  xDnsRecord *next;       // Next record in list, or NULL
};
```

Returned as a singly-linked list. Memory is owned by the library — valid only during the callback.

## Architecture

```text
┌───────────────────────────────────────────────────┐
│              xDnsClient                           │
│                                                   │
│  ┌─────────────┐  ┌─────────────┐                 │
│  │ UDP socket  │  │ Timer       │                 │
│  │ (port 53)   │  │ (timeout)   │                 │
│  └──────┬──────┘  └──────┬──────┘                 │
│         │                │                        │
│  ┌──────┴────────────────┴───────┐                │
│  │        Query table            │                │
│  │  ID → {name, callback, arg}   │                │
│  └───────────────────────────────┘                │
│                                                   │
│  ┌───────────────┐  ┌───────────────┐             │
│  │ Cache (TTL)   │  │ Nameservers   │             │
│  └───────────────┘  └───────────────┘             │
└───────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────┐
│              xDnsServer                           │
│                                                    │
│  ┌─────────────┐  ┌─────────────┐                 │
│  │ UDP listener │  │ Zone records│                 │
│  │ (port 53)    │  │ (map)       │                 │
│  └──────┬───────┘  └──────┬───────┘                │
│         │                 │                        │
│  ┌──────┴─────────────────┴──────┐                │
│  │     Query handler             │                │
│  │  zone → filter → forward      │                │
│  └──────┬────────────────────────┘                │
│         │                                          │
│  ┌──────┴──────┐  ┌─────────────┐                │
│  │ xDnsClient  │  │ Filter cb   │                │
│  │ (forwarder) │  │ (optional)  │                │
│  └─────────────┘  └─────────────┘                │
└───────────────────────────────────────────────────┘
```

## API Overview

### Client API

| Function | Description |
| --- | --- |
| `xDnsClientCreate(conf)` | Create a DNS client bound to the current event loop |
| `xDnsClientDestroy(client)` | Destroy client, cancel in-flight queries |
| `xDnsClientDo(client, name, type, cb, arg)` | Resolve a hostname asynchronously |

### Server API

| Function | Description |
| --- | --- |
| `xDnsServerCreate(conf)` | Create a DNS server (authoritative, forwarding, or hybrid) |
| `xDnsServerDestroy(server)` | Destroy server and release all resources |
| `xDnsServerListen(server, host, port)` | Start listening for queries on a UDP port |
| `xDnsServerPort(server)` | Return the actual bound port |
| `xDnsServerAddZone(server, zone)` | Attach a zone of authoritative records |

### Zone API

| Function | Description |
| --- | --- |
| `xDnsZoneCreate()` | Create an empty zone |
| `xDnsZoneDestroy(zone)` | Destroy a zone and free all records |
| `xDnsZoneAdd(zone, name, type, rdata, rdlen, ttl)` | Add a record to the zone |

### Callbacks

| Type | Signature | Description |
| --- | --- | --- |
| `xDnsCallback` | `void (*)(xErrno, const xDnsRecord *, void *)` | Client completion callback |
| `xDnsFilterFunc` | `int (*)(const char *, uint16_t, void *)` | Server filter — return 0=allow, non-zero=block |

## Usage Example

```c
#include <x/base/event.h>
#include <x/dns/dns.h>

static void on_resolved(xErrno err, const xDnsRecord *records, void *arg) {
    if (err != xErrno_Ok) return;
    for (const xDnsRecord *r = records; r; r = r->next) {
        printf("  [%d] %s\n", r->qtype, r->name);
    }
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();
    xEventLoopEnter(loop);

    xDnsClientConf conf = {};
    conf.nameservers[0] = "8.8.8.8";
    conf.timeout_ms = 5000;

    xDnsClient client = xDnsClientCreate(&conf);
    xDnsClientDo(client, "example.com", xDnsType_A | xDnsType_AAAA,
                 on_resolved, NULL);

    xEventLoopRun(loop);
    xDnsClientDestroy(client);
    xEventLoopDestroy(loop);
    return 0;
}
```

## Sub-pages

- [DNS Client API](client.md) — Resolver with caching and bitmask queries
- [DNS Server API](server.md) — Authoritative zones, forwarding, and filtering

## RFC Compliance

| RFC | Coverage |
|-----|----------|
| RFC 1034 | Query/response model, CNAME chains, caching |
| RFC 1035 | Packet format, name encoding, compression pointers |
| RFC 3596 | AAAA record type (QTYPE=28) |
| RFC 6891 | EDNS0 OPT record, UDP payload size 4096 |

## Relationship with Other Modules

- **xbase** — Depends on `xEventLoop`, `xSocket`, `xTimer`, `xMap` for async I/O, timeout, and data storage.
