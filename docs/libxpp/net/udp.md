# UDP

## Introduction

`xpp::net::UdpSocket` provides Promise-based async UDP. libx has no UDP API, so `UdpSocket` is built directly on `::socket()`, `::bind()`, and `io::AsyncFd`.

## Example — `.await()`

```cpp
#include <xpp/net/udp.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto server = xpp::net::UdpSocket::bind("127.0.0.1:9090").await().unwrap();

char buf[64];
auto result = server.recv_from(buf, sizeof(buf)).await();
// result.first  == bytes read
// result.second == peer SocketAddr
```

## Example — `co_await` (C++20)

```cpp
xpp::Promise<void> udp_echo(uint16_t server_port) {
    auto server_r = co_await xpp::net::UdpSocket::bind(
        ("127.0.0.1:" + std::to_string(server_port)).c_str());
    auto server = std::move(server_r).unwrap();

    auto client_r = co_await xpp::net::UdpSocket::bind("127.0.0.1:0");
    auto client = std::move(client_r).unwrap();
    auto target = server.local_addr().unwrap();

    auto recv_buf = std::make_shared<std::vector<char>>(64);
    auto recv_p = server.recv_from(recv_buf->data(), recv_buf->size());
    co_await client.send_to("ping", 4, target);

    auto result = co_await std::move(recv_p);
    // result.first == 4, result.second is client's address
}
```

## API Reference

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `bind(SocketAddr)` | `Promise<io::Result<UdpSocket>>` | Async bind (sync impl, resolves immediately) |
| `bind("host:port")` | `Promise<io::Result<UdpSocket>>` | Async bind with DNS for hostnames |
| `connect(addr)` | `xErrno` | Connect to peer (enables recv/send) |
| `recv(buf, len)` | `Promise<ssize_t>` | Connected-mode recv |
| `send(buf, len)` | `Promise<ssize_t>` | Connected-mode send |
| `recv_from(buf, len)` | `Promise<pair<ssize_t, SocketAddr>>` | Receive datagram + peer |
| `send_to(buf, len, target)` | `Promise<ssize_t>` | Send datagram to target |
| `try_recv(buf, len)` | `ssize_t` | Sync non-blocking recv |
| `try_send(buf, len)` | `ssize_t` | Sync non-blocking send |
| `try_recv_from(buf, len)` | `pair<ssize_t, Option<Addr>>` | Sync non-blocking recvfrom |
| `try_send_to(buf, len, target)` | `ssize_t` | Sync non-blocking sendto |
| `peek(buf, len)` | `Promise<ssize_t>` | Read without consuming (MSG_PEEK) |
| `peek_from(buf, len)` | `Promise<pair<ssize_t, Addr>>` | Peek + sender address |
| `readable()` | `Promise<void>` | Wait for readability |
| `writable()` | `Promise<void>` | Wait for writability |
| `take_error()` | `int` | Get & clear SO_ERROR |
| `broadcast()` | `io::Result<bool>` | Get SO_BROADCAST |
| `set_broadcast(bool)` | `io::Result<bool>` | Set SO_BROADCAST |
| `ttl()` | `io::Result<uint32_t>` | Get IP_TTL |
| `set_ttl(uint32_t)` | `io::Result<bool>` | Set IP_TTL |
| `peer_addr()` | `Option<SocketAddr>` | Connected peer address |
| `local_addr()` | `Option<SocketAddr>` | Bound address |
| `close()` | `void` | Close + deregister |
| `is_open()` | `bool` | Socket is active |

## How it works

`UdpSocket` creates a non-blocking UDP socket via `::socket(AF_INET, SOCK_DGRAM, 0)`, sets `O_NONBLOCK` + `FD_CLOEXEC`, binds, and registers with the event loop via `AsyncFd`.

`recv_from()` tries `::recvfrom` immediately (fast path). On EAGAIN, it waits for `readable()` and retries. Returns `Promise<std::pair<ssize_t, SocketAddr>>` — bytes read + peer address.

`send_to()` tries `::sendto` immediately. On EAGAIN, it waits for `writable()` and retries.

## Usage Examples

### UDP Echo — `.await()`

```cpp
auto server = xpp::net::UdpSocket::bind("127.0.0.1:9090").await().unwrap();

auto client = xpp::net::UdpSocket::bind("127.0.0.1:0").await().unwrap();

auto target = server.local_addr().unwrap();
client.send_to("ping", 4, target).await();

char buf[64];
auto result = server.recv_from(buf, sizeof(buf)).await();
// result.first == 4, result.second is client's address
```

### UDP Echo — `co_await` (C++20)

```cpp
auto server = (co_await xpp::net::UdpSocket::bind("127.0.0.1:9090")).unwrap();
auto client = (co_await xpp::net::UdpSocket::bind("127.0.0.1:0")).unwrap();
auto target = server.local_addr().unwrap();
co_await client.send_to("ping", 4, target);
char buf[64];
auto result = co_await server.recv_from(buf, sizeof(buf));
```

## Implementation Notes

- **Built from scratch** — libx has no UDP API. `UdpSocket` uses `::socket()` + `::bind()` + `AsyncFd` directly.
- **Buffer lifetime** — `buf` pointers passed to `recv_from`/`send_to` must remain valid until the returned Promise resolves.
- **Bind with DNS** — `bind("host:port")` tries `SocketAddr::parse` first (literal IP). If that fails, it splits on the last `:` and calls `lookup_host()` to resolve the hostname.
