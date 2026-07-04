## Why

xpp needs a net module for async TCP/UDP/DNS networking. libx already has TCP (`xTcpConnect`, `xTcpListener`, `xTcpConn`), DNS (`xDnsResolve`), TLS (`xTlsCtx`), and URL (`xUrlParse`) — all callback-based. With `io::AsyncFd` and `net::addr.h` already in place, wrapping these into Promise-based APIs is straightforward via the `adapt()` pattern.

Also fix two infrastructure issues discovered during net development:
- CMake target `xpp_core` should be `xpp` (no longer a separate compiled library)
- `cxx_std_17` should be `cxx_std_11` (Promise and all core types are C++11-compatible; only coroutine tests need C++20, already handled separately)

## What Changes

### Net module (`net/tcp.h`, `net/udp.h`, `net/dns.h`, `net/url.h`, `net/tls.h`)

- `TcpConn` — RAII wrapper around `xTcpConn` + `AsyncFd`. `connect()` via adapt (callback → PromiseResolver). `recv()`/`send()` delegate to `io::read()`/`io::write()` (fast-path syscall + EAGAIN readiness). TLS transparent via `TlsContext*` parameter (libx does handshake inside `xTcpConnect`).
- `TcpListener` — RAII wrapper around `xTcpListener`. `bind()` sync (creates listener). `accept()` via adapt (stores PromiseResolver, callback resolves on incoming connection).
- `UdpSocket` — built from `xSocketCreate(SOCK_DGRAM)` + `AsyncFd`. `recv_from()` returns `Promise<std::pair<ssize_t, SocketAddr>>`. `send_to()` returns `Promise<ssize_t>`.
- `resolve(hostname)` — DNS via adapt. Returns `Promise<std::vector<SocketAddr>>`.
- `Url` — RAII wrapper around `xUrl`. `parse()` returns `Result<Url>`. Accessors for scheme/host/port/path.
- `TlsConfig` / `TlsContext` — RAII wrappers around `xTlsConf` / `xTlsCtx`.

### Infrastructure fixes

- CMake: `xpp_core` → `xpp`, `cxx_std_17` → `cxx_std_11`
- `addr.h`: `inline const` static members → `static constexpr` (C++11-compatible)
- Header comments: "C++17-compatible" → "C++11-compatible" where appropriate

### Already done (in PR #30, this change builds on top)

- `net/addr.h` — Ipv4Addr, Ipv6Addr, IpAddr, SocketAddr (ported from moo, header-only, std::string)

## Capabilities

### New Capabilities
- `net-tcp`: Async TCP — `TcpConn` (connect/recv/send/peer_addr/local_addr), `TcpListener` (bind/accept)
- `net-udp`: Async UDP — `UdpSocket` (bind/recv_from/send_to)
- `net-dns`: Async DNS — `resolve(hostname)` → `Promise<vector<SocketAddr>>`
- `net-url`: URL parsing — `Url` (parse/scheme/host/port/path)
- `net-tls`: TLS configuration — `TlsConfig`, `TlsContext`

### Modified Capabilities
(none)

## Impact

- **New files**: `libxpp/xpp/net/tcp.h`, `libxpp/xpp/net/udp.h`, `libxpp/xpp/net/dns.h`, `libxpp/xpp/net/url.h`, `libxpp/xpp/net/tls.h`, test files
- **Modified files**: `libxpp/xpp/CMakeLists.txt` (rename target, std change, link xnet), `libxpp/xpp/net/addr.h` (inline const → constexpr)
- **Dependencies**: `libx/x/net` (xnet), `xpp/io/async_fd.h`, `xpp/net/addr.h`
- **No breaking changes**: entirely new module + infrastructure cleanup
