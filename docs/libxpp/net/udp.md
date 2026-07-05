# UDP

## Introduction

`xpp::net::UdpSocket` provides Promise-based async UDP. libx has no UDP API, so `UdpSocket` is built directly on `::socket()`, `::bind()`, and `io::AsyncFd`.

```cpp
#include <xpp/net/udp.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto server_r = xpp::net::UdpSocket::bind("127.0.0.1:9090").wait();
auto server = std::move(server_r).unwrap();

char buf[64];
auto result = server.recv_from(buf, sizeof(buf)).wait();
// result.first  == bytes read
// result.second == peer SocketAddr
```

## API Reference

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `bind(SocketAddr)` | `Promise<io::Result<UdpSocket>>` | Async bind (sync impl, resolves immediately) |
| `bind("host:port")` | `Promise<io::Result<UdpSocket>>` | Async bind with DNS for hostnames |
| `recv_from(buf, len)` | `Promise<pair<ssize_t, SocketAddr>>` | Receive datagram + peer |
| `send_to(buf, len, target)` | `Promise<ssize_t>` | Send datagram to target |
| `local_addr()` | `Option<SocketAddr>` | Bound address |
| `close()` | `void` | Close + deregister |
| `is_open()` | `bool` | Socket is active |

## How it works

`UdpSocket` creates a non-blocking UDP socket via `::socket(AF_INET, SOCK_DGRAM, 0)`, sets `O_NONBLOCK` + `FD_CLOEXEC`, binds, and registers with the event loop via `AsyncFd`.

`recv_from()` tries `::recvfrom` immediately (fast path). On EAGAIN, it waits for `readable()` and retries. Returns `Promise<std::pair<ssize_t, SocketAddr>>` — bytes read + peer address. C++11 callers use `std::tie`; C++17 callers may use structured bindings.

`send_to()` tries `::sendto` immediately. On EAGAIN, it waits for `writable()` and retries.

## Usage Examples

### UDP Echo

```cpp
auto server_r = xpp::net::UdpSocket::bind("127.0.0.1:9090").wait();
auto server = std::move(server_r).unwrap();

auto client_r = xpp::net::UdpSocket::bind("127.0.0.1:0").wait();
auto client = std::move(client_r).unwrap();

auto target = server.local_addr().unwrap();
client.send_to("ping", 4, target).wait();

char buf[64];
auto result = server.recv_from(buf, sizeof(buf)).wait();
// result.first == 4, result.second is client's address
```

## Coroutine Examples

```cpp
xpp::Promise<void> udp_echo(uint16_t server_port) {
    auto server_r = co_await xpp::net::UdpSocket::bind(("127.0.0.1:" + std::to_string(server_port)).c_str());
    auto server = std::move(server_r).unwrap();

    auto client_r = co_await xpp::net::UdpSocket::bind("127.0.0.1:0");
    auto client = std::move(client_r).unwrap();

    auto target = server.local_addr().unwrap();

    // Send + receive concurrently
    auto recv_buf = std::make_shared<std::vector<char>>(64);
    auto recv_p = server.recv_from(recv_buf->data(), recv_buf->size());
    co_await client.send_to("ping", 4, target);

    auto result = co_await std::move(recv_p);
    // result.first == 4, result.second is client's address
}
```

## Implementation Notes

- **Built from scratch** — libx has no UDP API. `UdpSocket` uses `::socket()` + `::bind()` + `AsyncFd` directly.

- **Buffer lifetime** — `buf` pointers passed to `recv_from`/`send_to` must remain valid until the returned Promise resolves.

- **Bind with DNS** — `bind("host:port")` tries `SocketAddr::parse` first (literal IP). If that fails, it splits on the last `:` and calls `lookup_host()` to resolve the hostname, then binds to the first resolved address.
