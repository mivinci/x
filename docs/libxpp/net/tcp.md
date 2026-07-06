# TCP

## Introduction

`xpp::net::TcpStream` and `TcpListener` provide Promise-based async TCP, wrapping libx's `xTcpConn` and `xTcpListener`. I/O uses `io::AsyncFd` for readiness — fast-path syscall + EAGAIN wait.

```cpp
#include <xpp/net/tcp.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

// Client
auto conn_r = xpp::net::TcpStream::connect("127.0.0.1:8080").wait();
auto conn = std::move(conn_r).unwrap();
conn.write("hello", 5).wait();

char buf[64];
ssize_t n = conn.read(buf, sizeof(buf)).wait();
```

## TcpStream

### API Reference

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `connect("host:port", tls = none)` | `Promise<io::Result<TcpStream>>` | Async connect ("IP:port" or "hostname:port", with optional TLS) |
| `connect(SocketAddr, tls = none)` | `Promise<io::Result<TcpStream>>` | Connect by address (no DNS) |
| `read(buf, len)` | `Promise<ssize_t>` | Async read via `io::read` |
| `write(buf, len)` | `Promise<ssize_t>` | Async write via `io::write` |
| `try_read(buf, len)` | `ssize_t` | Sync non-blocking read, -1/EAGAIN if none |
| `try_write(buf, len)` | `ssize_t` | Sync non-blocking write, -1/EAGAIN if full |
| `peek(buf, len)` | `Promise<ssize_t>` | Read without consuming (MSG_PEEK) |
| `readable()` | `Promise<void>` | Resolve when fd is readable |
| `writable()` | `Promise<void>` | Resolve when fd is writable |
| `take_error()` | `int` | Get & clear SO_ERROR, 0 if none |
| `nodelay()` / `set_nodelay(bool)` | `io::Result<bool>` | TCP_NODELAY get/set |
| `ttl()` / `set_ttl(uint32_t)` | `io::Result<uint32_t>` / `io::Result<bool>` | IP_TTL get/set |
| `linger()` / `set_linger(int32_t)` | `io::Result<int32_t>` / `io::Result<bool>` | SO_LINGER get/set |
| `peer_addr()` | `Option<SocketAddr>` | Peer's address |
| `local_addr()` | `Option<SocketAddr>` | Local address |
| `close()` | `void` | Close + deregister |
| `is_open()` | `bool` | Connection is active |

### How it works

`TcpStream` wraps `xTcpConn` + `io::AsyncFd`. The fd is extracted from `xTcpConnSocket()` and registered with the event loop. `read()`/`write()` delegate to `io::read()`/`io::write()` (fast-path `::read`/`::write` + EAGAIN readiness wait).

`connect()` uses `async() + new TcpConnectAdapter` — the adapter self-deletes in its callback because `xTcpConnect` has no cancel API. If the Promise is dropped before connect completes, the adapter stays alive until the callback fires (bounded by libx's 10s connect timeout) and resolves to a no-op via `ArcWeak`.

### Connect with TLS

Pass `Option<const TlsContext&>` to enable TLS. The handshake is transparent:

```cpp
xpp::net::TlsContext tls(xpp::net::TlsConfig::client());
auto conn = xpp::net::TcpStream::connect("example.com:443", tls).wait();
// conn.read() / conn.write() transparently encrypt/decrypt
```

## TcpListener

### API Reference

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `bind(SocketAddr)` | `Promise<io::Result<TcpListener>>` | Async bind (sync impl, resolves immediately) |
| `bind("host:port")` | `Promise<io::Result<TcpListener>>` | Async bind with DNS for hostnames |
| `accept()` | `Promise<pair<TcpStream, SocketAddr>>` | Next incoming connection + peer address |
| `local_addr()` | `Option<SocketAddr>` | Bound address |
| `close()` | `void` | Stop listening |
| `is_open()` | `bool` | Listener is active |

### How it works

`TcpListener` wraps `xTcpListener`. The underlying state (listener handle + pending resolver) lives in a `shared_ptr<Impl>` so the libx callback's `void* arg` stays stable across moves.

`accept()` returns both the stream and the peer address in a `pair<TcpStream, SocketAddr>` — the libx callback provides the address for free, no extra syscall.

`local_addr()` uses `xTcpListenerSocket` (new libx API) to get the listener's fd, then `getsockname()` to retrieve the bound address.

## Usage Examples

### TCP Echo Server

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto listener_r = xpp::net::TcpListener::bind("0.0.0.0:8080").wait();
ASSERT_TRUE(listener_r.is_ok());
auto listener = std::move(listener_r).unwrap();

auto session = [](xpp::net::TcpStream conn) {
    auto buf = std::make_shared<std::vector<char>>(1024);
    auto conn_ptr = std::make_shared<xpp::net::TcpStream>(std::move(conn));
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
auto conn = xpp::net::TcpStream::connect("example.com:443", tls).wait();
conn.write("GET / HTTP/1.0\r\n\r\n", 18).wait();

char buf[4096];
ssize_t n = conn.read(buf, sizeof(buf)).wait();
```

## Coroutine Examples

`Promise<T>` is a coroutine return type — functions returning `Promise<T>` can use `co_await` / `co_return`. Requires C++20.

### Echo server (coroutine)

```cpp
xpp::Promise<void> session(xpp::net::TcpStream conn) {
    auto buf = std::make_shared<std::vector<char>>(1024);
    ssize_t n = co_await conn.read(buf->data(), buf->size());
    co_await conn.write(buf->data(), static_cast<size_t>(n));
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
    auto conn = co_await xpp::net::TcpStream::connect("example.com:443", tls);

    co_await conn.write("GET / HTTP/1.0\r\n\r\n", 18);

    char buf[4096];
    ssize_t n = co_await conn.read(buf, sizeof(buf));
    printf("%.*s\n", (int)n, buf);
}
```

### Concurrent server + client

```cpp
xpp::Promise<void> echo_server(xpp::net::TcpListener listener) {
    auto conn = co_await listener.accept();
    char buf[64];
    ssize_t n = co_await conn.read(buf, sizeof(buf));
    co_await conn.write(buf, n);
}

xpp::Promise<void> echo_client(uint16_t port) {
    auto conn = co_await xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());
    co_await conn.write("hello", 5);
    char buf[64];
    ssize_t n = co_await conn.read(buf, sizeof(buf));
    // n == 5, buf == "hello"
}

// Drive both concurrently:
xpp::all(echo_server(std::move(listener)), echo_client(port)).wait();
```

## Implementation Notes

- **TcpConnectAdapter self-deletes** — `xTcpConnect` has no cancel API. The adapter is heap-allocated (`new`) and self-deletes in its callback. If the Promise is dropped first, the callback still fires (bounded by 10s timeout) and resolves to a no-op via `ArcWeak`.

- **TcpListener uses shared_ptr<Impl>** — `xTcpListenerCreate` stores a `void* arg` pointer. The `Impl` struct is heap-allocated and stable, so moves don't dangle the callback arg.

- **Buffer lifetime** — `buf` pointers passed to `recv`/`send` must remain valid until the returned Promise resolves.
