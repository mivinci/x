# DNS

## Introduction

`xpp::net::lookup_host()` provides async hostname resolution, wrapping libx's `xDnsResolve`. Returns a vector of `SocketAddr`.

## Example — `.await()`

```cpp
#include <xpp/net/dns.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto addrs = xpp::net::lookup_host("example.com").await();
for (const auto &addr : addrs) {
    printf("%s\n", addr.to_string().c_str());
}
```

## Example — `co_await` (C++20)

```cpp
xpp::Promise<void> connect_to(const char *hostname) {
    auto addrs = co_await xpp::net::lookup_host(hostname);
    if (addrs.empty()) { co_return; }
    auto conn = co_await xpp::net::TcpStream::connect(addrs[0]);
    co_await conn.write("ping", 4);
}
```

## API Reference

| Function | Returns | Description |
| ---------- | --------- | ------------- |
| `lookup_host(hostname)` | `Promise<vector<SocketAddr>>` | Async DNS resolution |

Resolves to an empty vector on failure (hostname not found, DNS error, etc.).

## How it works

`lookup_host()` uses `adapt<vector<SocketAddr>, LookupHostAdapter>()`. The adapter calls `xDnsResolve` in its constructor and `xDnsCancel` in its destructor. Dropping the Promise mid-query cancels the query safely.

## Usage Examples

### Resolve and connect — `.await()`

```cpp
auto addrs = xpp::net::lookup_host("example.com").await();
if (!addrs.empty()) {
    auto conn = xpp::net::TcpStream::connect(addrs[0]).await();
}
```

### Resolve and connect — `co_await` (C++20)

```cpp
xpp::Promise<void> connect_to(const char *hostname) {
    auto addrs = co_await xpp::net::lookup_host(hostname);
    if (addrs.empty()) { printf("host not found\n"); co_return; }
    auto conn = co_await xpp::net::TcpStream::connect(addrs[0]);
}
```

### Concurrent resolution — `.await()` with .then()

```cpp
xpp::net::lookup_host("example.com").then([](std::vector<xpp::net::SocketAddr> addrs) {
    if (addrs.empty()) return xpp::resolve(xpp::net::TcpStream());
    return xpp::net::TcpStream::connect(addrs[0]);
}).await();
```

### Concurrent multi-host resolution — `co_await` (C++20)

```cpp
xpp::Promise<void> resolve_both() {
    auto a = xpp::net::lookup_host("example.com");
    auto b = xpp::net::lookup_host("example.org");
    auto pair = co_await xpp::all(std::move(a), std::move(b));
    printf("example.com: %zu addrs, example.org: %zu addrs\n",
           pair.first.size(), pair.second.size());
}
```

## Implementation Notes

- **Empty vector on error** — libx's DNS callback provides `xDnsResult` with an error field. On error, the adapter resolves with an empty vector (not a rejection). Callers should check `addrs.empty()`.
