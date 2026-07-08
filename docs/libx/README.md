# LibX

libx is organized into nine libraries, layered from low-level core primitives up to high-level async networking, filesystem I/O, crypto, and DNS.

```text
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                    │
├──────────────────────┬──────────────────┬───────────────┤
│   xhttp              │  xp2p            │   xdns        │
│   HTTP Client/Server │  ICE/STUN/TURN   │   DNS Client  │
│   WebSocket / SSE    │  Peer Connection │   DNS Server  │
├──────────────────────┼──────────────────┼───────────────┤
│   xnet               │   xlog           │   xfs         │
│   URL / TCP / TLS    │   Async Logging  │   Async FS    │
├──────────────────────┴──────────────────┴───────────────┤
│   xbuf — Linear / Ring / Block-Chain Buffer             │
├──────────────────────┬──────────────────────────────────┤
│   xbase              │   xcrypto                        │
│   Event Loop / Timer │   SHA-1/256 MD5 CRC-32           │
│   Task / Memory / IO │   HMAC / UUID                    │
└──────────────────────┴──────────────────────────────────┘
```

## Overview

| Library | Description |
| ------- | ----------- |
| **[xbase](base/README.md)** | Core primitives — event loop, timers, tasks, async sockets, memory, lock-free data structures |
| **[xbuf](buf/README.md)** | Buffer primitives — linear, ring, and block-chain I/O buffers |
| **[xcrypto](crypto/README.md)** | Cryptographic primitives — SHA-1, SHA-256 (OpenSSL / mbedTLS / builtin), MD5, CRC-32, HMAC, UUID (v4/v5/v7) |
| **[xnet](net/README.md)** | Networking primitives — URL parser, async DNS resolution, TCP, shared TLS configuration types |
| **[xlog](log/README.md)** | Async logging — MPSC queue, timer/pipe flush, log rotation |
| **[xhttp](http/README.md)** | Async HTTP client & server — libcurl multi-socket client with SSE streaming, HTTP/1.1 & HTTP/2 async server with TLS, WebSocket server & client |
| **[xdns](dns/README.md)** | Async DNS client & server — protocol-native resolver over UDP with TTL caching, bitmask queries, authoritative zones, forwarding, and query filtering |
| **[xp2p](p2p/README.md)** | P2P connectivity — ICE agent, STUN/TURN client, SDP codec, NAT traversal |
| **[xfs](fs/README.md)** | Async filesystem I/O — open, close, read, write, stat, mkdir, rmdir, unlink, rename via thread pool offload with dual async/sync modes |

## Dependency Order

```text
Level 0 (no deps)      : atomic.h, error.h, time.h
Level 1 (atomic only)  : heap.h, mpsc.h
Level 2 (Level 0-1)    : memory.h, random.h, log.h, backtrace.h, buf.h, ring.h
Level 3 (Level 0-2)    : event.h, io.h, url.h, tls.h
Level 4 (event loop)   : timer.h, task.h, socket.h, fs.h, dns.h (xnet), tcp.h, logger.h, client.h, server.h, ws.h
Level 5 (xbase+xnet)   : ice_agent.h, stun_msg.h, stun_attr.h, stun_txn.h, turn_client.h, sdp.h, dns.h (xdns)
Level ∞ (standalone)   : sha1.h, sha256.h, md5.h, crc32.h, hmac.h, uuid.h (xcrypto — depends only on xbase error codes)
```
