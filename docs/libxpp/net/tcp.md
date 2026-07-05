# TCP

## Introduction

`xpp::net::TcpConn` and `TcpListener` provide Promise-based async TCP, wrapping libx's `xTcpConn` and `xTcpListener`. I/O uses `io::AsyncFd` for readiness — fast-path syscall + EAGAIN wait.

```cpp
#include <xpp/net/tcp.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

// Client
auto conn = xpp::net::TcpConn::connect("127.0.0.1", 8080).wait();
conn.send("hello", 5).wait();

char buf[64];
ssize_t n = conn.recv(buf, sizeof(buf)).wait();
```

## TcpConn

### API Reference

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `connect(host, port, tls = none)` | `Promise<TcpConn>` | Async connect (with optional TLS) |
| `connect(SocketAddr, tls = none)` | `Promise<TcpConn>` | Connect by address (no DNS) |
| `recv(buf, len)` | `Promise<ssize_t>` | Async read via `io::read` |
| `send(buf, len)` | `Promise<ssize_t>` | Async write via `io::write` |
| `peer_addr()` | `Option<SocketAddr>` | Peer's address |
| `local_addr()` | `Option<SocketAddr>` | Local address |
| `close()` | `void` | Close + deregister |
| `is_open()` | `bool` | Connection is active |

### How it works

`TcpConn` wraps `xTcpConn` + `io::AsyncFd`. The fd is extracted from `xTcpConnSocket()` and registered with the event loop. `recv()`/`send()` delegate to `io::read()`/`io::write()` (fast-path `::read`/`::write` + EAGAIN readiness wait).

`connect()` uses `async() + new TcpConnectAdapter` — the adapter self-deletes in its callback because `xTcpConnect` has no cancel API. If the Promise is dropped before connect completes, the adapter stays alive until the callback fires (bounded by libx's 10s connect timeout) and resolves to a no-op via `ArcWeak`.

### Connect with TLS

Pass `Option<const TlsContext&>` to enable TLS. The handshake is transparent:

```cpp
xpp::net::TlsContext tls(xpp::net::TlsConfig::client());
auto conn = xpp::net::TcpConn::connect("example.com", 443, tls).wait();
// conn.recv() / conn.send() transparently encrypt/decrypt
```

## TcpListener

### API Reference

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `bind(SocketAddr)` | `Promise<io::Result<TcpListener>>` | Async bind (sync impl, resolves immediately) |
| `bind("host:port")` | `Promise<io::Result<TcpListener>>` | Async bind with DNS for hostnames |
| `accept()` | `Promise<TcpConn>` | Next incoming connection |
| `close()` | `void` | Stop listening |
| `is_open()` | `bool` | Listener is active |

### How it works

`TcpListener` wraps `xTcpListener`. The underlying state (listener handle + pending resolver) lives in a `shared_ptr<Impl>` so the libx callback's `void* arg` stays stable across moves.

`accept()` stores a `PromiseResolver<TcpConn>` in `Impl::pending`. When `on_accept` fires, it resolves `pending` (or closes the connection if no one is waiting).

## Usage Examples

### TCP Echo Server

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto listener_r = xpp::net::TcpListener::bind("0.0.0.0:8080").wait();
ASSERT_TRUE(listener_r.is_ok());
auto listener = std::move(listener_r).unwrap();

auto session = [](xpp::net::TcpConn conn) {
    auto buf = std::make_shared<std::vector<char>>(1024);
    auto conn_ptr = std::make_shared<xpp::net::TcpConn>(std::move(conn));
    return conn_ptr->recv(buf->data(), buf->size())
        .then([conn_ptr, buf](ssize_t n) mutable {
            return conn_ptr->send(buf->data(), static_cast<size_t>(n));
        })
        .then([conn_ptr](ssize_t) mutable {});
};

session(listener.accept().wait()).wait();
```

### TCP Client with TLS

```cpp
xpp::net::TlsContext tls(xpp::net::TlsConfig::client());
auto conn = xpp::net::TcpConn::connect("example.com", 443, tls).wait();
conn.send("GET / HTTP/1.0\r\n\r\n", 18).wait();

char buf[4096];
ssize_t n = conn.recv(buf, sizeof(buf)).wait();
```

## Coroutine Examples

`Promise<T>` is a coroutine return type — functions returning `Promise<T>` can use `co_await` / `co_return`. Requires C++20.

### Echo server (coroutine)

```cpp
xpp::Promise<void> session(xpp::net::TcpConn conn) {
    auto buf = std::make_shared<std::vector<char>>(1024);
    ssize_t n = co_await conn.recv(buf->data(), buf->size());
    co_await conn.send(buf->data(), static_cast<size_t>(n));
}

xpp::Promise<void> server(xpp::net::TcpListener listener) {
    while (listener.is_open()) {
        auto conn = co_await listener.accept();
        co_await session(std::move(conn));
    }
}
```

### Client with TLS (coroutine)

```cpp
xpp::Promise<void> fetch() {
    xpp::net::TlsContext tls(xpp::net::TlsConfig::client());
    auto conn = co_await xpp::net::TcpConn::connect("example.com", 443, tls);

    co_await conn.send("GET / HTTP/1.0\r\n\r\n", 18);

    char buf[4096];
    ssize_t n = co_await conn.recv(buf, sizeof(buf));
    printf("%.*s\n", (int)n, buf);
}
```

### Concurrent server + client

```cpp
xpp::Promise<void> echo_server(xpp::net::TcpListener listener) {
    auto conn = co_await listener.accept();
    char buf[64];
    ssize_t n = co_await conn.recv(buf, sizeof(buf));
    co_await conn.send(buf, n);
}

xpp::Promise<void> echo_client(uint16_t port) {
    auto conn = co_await xpp::net::TcpConn::connect("127.0.0.1", port);
    co_await conn.send("hello", 5);
    char buf[64];
    ssize_t n = co_await conn.recv(buf, sizeof(buf));
    // n == 5, buf == "hello"
}

// Drive both concurrently:
xpp::all(echo_server(std::move(listener)), echo_client(port)).wait();
```

## Implementation Notes

- **TcpConnectAdapter self-deletes** — `xTcpConnect` has no cancel API. The adapter is heap-allocated (`new`) and self-deletes in its callback. If the Promise is dropped first, the callback still fires (bounded by 10s timeout) and resolves to a no-op via `ArcWeak`.

- **TcpListener uses shared_ptr<Impl>** — `xTcpListenerCreate` stores a `void* arg` pointer. The `Impl` struct is heap-allocated and stable, so moves don't dangle the callback arg.

- **Buffer lifetime** — `buf` pointers passed to `recv`/`send` must remain valid until the returned Promise resolves.
