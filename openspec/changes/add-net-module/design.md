## Context

libx's net module provides callback-based async TCP, DNS, TLS, and sync URL parsing. With `io::AsyncFd` (reactive I/O → Promise) and `net::addr.h` (address types) already implemented, the remaining work is wrapping libx APIs into Promise-based xpp APIs.

Two infrastructure issues also need fixing: CMake target name (`xpp_core` → `xpp`) and C++ standard (`cxx_std_17` → `cxx_std_11`).

## Goals / Non-Goals

**Goals:**
- `TcpConn`: connect (adapt), recv/send (via AsyncFd), peer_addr/local_addr, RAII close
- `TcpListener`: bind (sync), accept (adapt — stores resolver, callback resolves)
- `UdpSocket`: bind (sync), recv_from (AsyncFd + recvfrom), send_to (AsyncFd + sendto)
- `resolve(hostname)` → `Promise<vector<SocketAddr>>` (adapt)
- `Url`: RAII wrapper, parse → Result<Url>
- `TlsConfig` / `TlsContext`: RAII wrappers
- TLS transparent: pass `TlsContext*` to `TcpConn::connect()`, libx handles handshake
- CMake: rename `xpp_core` → `xpp`, `cxx_std_17` → `cxx_std_11`
- `addr.h`: `inline const` → `static constexpr` (C++11)

**Non-Goals:**
- `AsyncRead`/`AsyncWrite` traits (like Rust's tokio traits)
- Unix sockets (can be added later)
- `split()` for concurrent read/write on same TcpConn
- TLS server handshake (libx supports it, but API not exposed yet)
- HTTP client/server (separate module)

## Decisions

### D1: TcpConn wraps xTcpConn + AsyncFd

```
TcpConn
├── xTcpConn m_conn     (libx handle, owns socket + transport)
├── int m_fd            (raw fd, extracted from xTcpConnSocket)
└── AsyncFd m_async     (registered with event loop for readiness)
```

`recv()`/`send()` delegate to `io::read(m_async, buf, len)` / `io::write(m_async, buf, len)`. Fast-path syscall (zero Promise overhead), slow-path EAGAIN → readiness wait.

### D2: All async ops use adapt()

| Operation | libx API | Adapter | Resolves with |
|-----------|----------|---------|---------------|
| TCP connect | `xTcpConnect(cb)` | `TcpConnectAdapter` | `TcpConn` |
| TCP accept | `xTcpListenerFunc(cb)` | `TcpAcceptAdapter` | `TcpConn` |
| DNS resolve | `xDnsCallback(cb)` | `DnsResolveAdapter` | `vector<SocketAddr>` |

TCP connect and DNS adapters self-delete in callback (libx doesn't support cancellation integrated with AdapterPromiseNode lifecycle). TCP accept adapter stores resolver in TcpListener (like AsyncFd's readable/writable pattern).

### D3: UdpSocket built from scratch

libx has no UDP API. `UdpSocket` uses:
- `xSocketCreate(AF_INET, SOCK_DGRAM, 0)` — create non-blocking UDP socket
- `AsyncFd` — register with event loop for readiness
- `recv_from()`: try `::recvfrom()`, EAGAIN → `readable().then(recvfrom)`
- `send_to()`: try `::sendto()`, EAGAIN → `writable().then(sendto)`

`recv_from()` returns `Promise<std::pair<ssize_t, SocketAddr>>` — bytes + peer address. C++11 users use `std::tie`, C++17 users use structured bindings.

### D4: TLS transparent (Option A)

```cpp
static Promise<TcpConn> connect(const char* host, uint16_t port,
                                const TlsContext* tls = nullptr);
```

libx's `xTcpConnect` already does TLS handshake when `xTcpConnectConf.tls_ctx` is set. The resulting `xTcpConn` transparently encrypts/decrypts. No separate `TlsConn` type needed.

### D5: C++ standard = C++11

Promise and all core types are C++11-compatible. `cxx_std_17` was incorrect. Change to `cxx_std_11`. Coroutine tests already handled separately (CMake detects "coroutine" in test name → C++20).

`addr.h` static constants: `inline const` (C++17) → `static constexpr` (C++11). The constructors are already `constexpr`, so this works.

### D6: CMake target rename

`xpp_core` → `xpp`. Originally `xpp_core` was a static library (had `panic.cpp`). Now everything is header-only (INTERFACE). No need for `_core` suffix.

## Risks / Trade-offs

- **[UdpSocket from scratch]** No libx UDP support — we build directly on `xSocketCreate` + `AsyncFd`. More code but full control.
- **[TcpConnectAdapter self-deletes]** `xTcpConnect` has no cancel API. Adapter self-deletes in callback. If Promise is destroyed before connect completes, the adapter leaks until the callback fires (then resolves to no-op via ArcWeak). Acceptable — connect timeout in libx (default 10s) bounds the leak.
- **[TcpListener move]** `xTcpListenerCreate` stores a `void* arg` pointer to `TcpListener`. After move, the pointer dangles. Workaround: don't move TcpListener after calling accept(). In practice, listeners are created and used in place.
- **[recv_from buffer lifetime]** `buf` pointer must survive until Promise resolves (same contract as `io::read`). Documented.
