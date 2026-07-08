# Take

## Introduction

`xpp::io::Take<R>` wraps any `AsyncReader` with a byte limit. Once the limit is reached, `read()` returns 0 (EOF) regardless of whether the inner reader has more data. Essential for HTTP `Content-Length` parsing and protocol frame boundaries where the reader must not consume beyond a known byte limit.

Satisfies the `AsyncReader` concept — composable with `io::read_all`, `io::copy`, `BufReader`, or nested in another `Take`. Takes ownership of the inner reader via move semantics. Coroutine (C++20) or struct+move fallback (C++11).

```cpp
#include <xpp/io/take.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto conn = xpp::net::TcpStream::connect("127.0.0.1:9090").await().unwrap();
xpp::io::Take<xpp::net::TcpStream> body(std::move(conn), 128);

// Reads exactly 128 bytes, then EOF
auto data = co_await xpp::io::read_all(body);
```

## How it works

```cpp
Take<R> { m_reader, m_remaining }

read(buf, len):
  m_remaining == 0? → co_return 0 (EOF)
  m_remaining > 0?  → cap len to m_remaining, forward to inner
  inner returns n  → m_remaining -= n, return n
```

A simple counter tracks remaining bytes. Each `read()` delegates to the inner reader with a capped length. Once the counter hits zero, all subsequent reads return 0 without touching the inner reader.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `Take(R reader, size_t limit)` | | Take ownership and set byte limit |
| `read(buf, len)` | `Promise<ssize_t>` | Read up to `len` bytes, capped by remaining limit |
| `remaining()` | `size_t` | Bytes remaining before EOF |

## Usage Examples

### HTTP body with Content-Length

```cpp
xpp::Promise<void> read_body(xpp::net::TcpStream conn, size_t content_length) {
    xpp::io::Take<xpp::net::TcpStream> body(std::move(conn), content_length);
    auto data = co_await xpp::io::read_all(body);
    // data.size() ≤ content_length
}
```

### Protocol frame boundary

```cpp
xpp::Promise<void> read_frame(xpp::net::TcpStream conn) {
    // Read 4-byte header
    char header[4];
    co_await conn.read(header, 4);
    uint32_t body_len = ntohl(*reinterpret_cast<uint32_t *>(header));

    // Read exactly body_len bytes — won't consume next frame
    xpp::io::Take<xpp::net::TcpStream> body(std::move(conn), body_len);
    auto data = co_await xpp::io::read_all(body);
}
```

### Read limit hits zero

```cpp
xpp::io::Take<xpp::net::TcpStream> limit(std::move(conn), 5);
char buf[10];

ssize_t n1 = co_await limit.read(buf, 5);
// n1 == 5 (if 5 bytes available), remaining == 0

ssize_t n2 = co_await limit.read(buf, 10);
// n2 == 0 (EOF), inner reader untouched
```

## Examples

All examples above show coroutine usage (C++20). For C++11, `Take` works identically — the `.read()` method uses `.then()` instead of `co_await` internally. See [Utilities](util.md) for composing `Take` with `io::read_all` and `io::copy`.
