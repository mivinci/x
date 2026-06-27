# xdns — Asynchronous DNS

## Introduction

**xdns** is libx's DNS module, providing both a **client** (resolver) and **server** (authoritative/forwarding). Unlike `libx/x/net/dns.c` which offloads `getaddrinfo()` to a thread pool, xdns implements the DNS protocol directly over UDP — truly async, no thread pool, no blocking calls.

All I/O is driven by xbase's event loop. The module depends on `xbase` only.

## Key Features

- **Truly async** — DNS queries over UDP, no `getaddrinfo`, no thread pool
- **Client** — resolve A/AAAA/CNAME records with TTL caching and retry
- **Server** — authoritative zones, forwarding, and query filtering
- **Bitmask queries** — `xDnsType_A | xDnsType_AAAA` sends parallel queries, merges results
- **RFC compliant** — follows RFC 1034, RFC 1035, RFC 3596, RFC 6891 (EDNS0)
- **Cross-platform** — resolv.conf on POSIX, GetNetworkParams on Windows

## Architecture

```
┌───────────────────────────────────────────────────┐
│              xDnsClient                             │
│                                                    │
│  ┌─────────────┐  ┌─────────────┐                 │
│  │ UDP socket  │  │ Timer       │                 │
│  │ (port 53)   │  │ (timeout)   │                 │
│  └──────┬──────┘  └──────┬──────┘                 │
│         │                 │                        │
│  ┌──────┴─────────────────┴──────┐                │
│  │        Query table            │                │
│  │  ID → {name, callback, arg}   │                │
│  └───────────────────────────────┘                │
│                                                    │
│  ┌───────────────┐  ┌───────────────┐            │
│  │ Cache (TTL)   │  │ Nameservers   │            │
│  └───────────────┘  └───────────────┘            │
└───────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────┐
│              xDnsServer                             │
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

## Modules

- [Client API](client.md) — xDnsClient resolver with caching and bitmask queries
- [Server API](server.md) — xDnsServer with authoritative zones, forwarding, and filtering
