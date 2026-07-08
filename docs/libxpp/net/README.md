# Net

## Introduction

`xpp::net` provides Promise-based async TCP, UDP, DNS, URL parsing, and TLS configuration — wrapping libx's C APIs into C++ types.

```cpp
#include <xpp/net/tcp.h>
#include <xpp/net/udp.h>
#include <xpp/net/url.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

using xpp::net::TcpStream;
using xpp::net::TcpListener;

// TCP echo server + client in one chain
auto server = TcpListener::bind("127.0.0.1:9090").await().unwrap();
auto client_p = TcpStream::connect("127.0.0.1:9090").then([](TcpStream c) {
    return c.write("hi", 2).then([c](ssize_t) mutable {
        char buf[64];
        return c.read(buf, 64);
    });
});
client_p.await();
```

## Design Philosophy

1. **Promise-based, poll-driven** — All async ops return `Promise<T>`. `wait()` drives the event loop; no separate runtime or reactor thread.

2. **Fast-path syscall + EAGAIN readiness** — `recv`/`send`/`recv_from`/`send_to` try the syscall immediately. On EAGAIN, they wait for readiness via `AsyncFd` and retry. Zero Promise overhead when data is available.

3. **adapt() for one-shot ops** — `lookup_host()` uses `adapt<T, Adapter>()` — the adapter starts the async op in its constructor, cancels in its destructor, and the `AdapterPromiseNode` owns the adapter. `TcpStream::connect()` uses `async() + new` (self-deleting adapter) because libx's `xTcpConnect` has no cancel API.

4. **TLS is transparent** — Pass `Option<const TlsContext&> = none` to `TcpStream::connect()` to enable TLS. libx's `xTcpConnect` does the handshake; the resulting `TcpStream` transparently encrypts/decrypts. No separate `TlsConn` type.

5. **RAII everywhere** — `TcpStream`, `TcpListener`, `UdpSocket`, `Url`, `TlsContext` all close/free their underlying resources in destructors. Move-only.

6. **C++11-compatible** — All headers compile as C++11. `std::pair` + `std::tie` for `recv_from` results (C++17 users may use structured bindings).

## Architecture

```text
TCP                           UDP                    DNS                URL/TLS
├── TcpStream                   ├── UdpSocket          ├── lookup_host()  ├── Url (sync)
│   ├── xTcpConn (libx)       │   ├── int m_fd       │   └── adapt()    │   └── xUrl
│   ├── AsyncFd (readiness)   │   └── AsyncFd        └── LookupHostAdapter  ├── TlsConfig
│   ├── connect via async()+new  └── bind/recv_from/send_to                └── TlsContext
├── TcpListener                                                              (RAII)
│   ├── xTcpListener
│   └── accept via adapt()
└── TLS via TlsContext
```

## Modules

- [TCP](tcp.md) — `TcpStream` and `TcpListener`: Promise-based async TCP.
- [UDP](udp.md) — `UdpSocket`: async UDP from scratch (libx has no UDP API).
- [DNS](dns.md) — `lookup_host()`: async hostname resolution.
- [URL](url.md) — `Url`: RAII wrapper around `xUrl` with structured errors.
- [TLS](tls.md) — `TlsConfig` and `TlsContext`: RAII TLS configuration.

`bind` methods return `Promise<io::Result<T, io::Error>>` — see [I/O Error](../io/error.md) for the error type.

## Comparison with tokio::net

| Aspect | xpp::net | tokio::net |
| -------- | ---------- | ------------ |
| Async model | Poll-based Promise + `wait()` | `async fn` + `.await` |
| TCP connect | `Promise<TcpStream>` (async()+new) | `Future<Result<TcpStream>>` |
| Readiness | `AsyncFd` (edge-triggered) | `mio` (edge-triggered) |
| Fast path | `::read` + EAGAIN → readiness | `read` + EAGAIN → readiness |
| TLS | `Option<const TlsContext&>` to `connect()` | `TlsConnector::connect()` |
| DNS | `lookup_host()` → `Promise<vector<SocketAddr>>` | `lookup_host()` → `Future<impl Iterator>` |
| UDP | `recv_from` → `Promise<pair<ssize_t, SocketAddr>>` | `recv_from` → `Future<Result<(usize, SocketAddr)>>` |
| Bind | Async (`Promise<io::Result<T>>`, DNS for hostnames) | Async (`ToSocketAddrs` may resolve) |
| Error type | `io::Error` (4 bytes, niche-optimized) | `std::io::Error` (heap-allocated) |
| Threading | Single-threaded | Multi-threaded runtime |
