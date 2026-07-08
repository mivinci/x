# BufReader

## Introduction

`xpp::io::BufReader<R>` wraps any `AsyncReader` with an internal 8KB buffer. Small reads copy from the buffer with zero I/O overhead; large reads (≥ 8KB) bypass the buffer entirely. Reduces per-call Promise overhead for byte-at-a-time parsing over TCP.

Satisfies the `AsyncReader` concept — composable with `io::read_all`, `io::copy`, or nested in another `BufReader`. Takes ownership of the inner reader via move semantics. Works with `.await()`, `co_await` (C++20), or `.then()` chains (C++11).

## Example — `.await()`

```cpp
#include <xpp/io/buf_reader.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto conn = xpp::net::TcpStream::connect("127.0.0.1:9090").await().unwrap();
xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));

// Small reads: copy from buffer, zero per-call Promise overhead
char c;
buf.read(&c, 1).await();  // fills buffer from TCP
buf.read(&c, 1).await();  // copies from buffer
```

## Example — `co_await` (C++20)

```cpp
auto conn = xpp::net::TcpStream::connect("127.0.0.1:9090").await().unwrap();
xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));

char c;
co_await buf.read(&c, 1);  // fills buffer from TCP
co_await buf.read(&c, 1);  // copies from buffer
```

## How it works

```text
read(buf, len)
    │
    ├── buffer has data? → memcpy from m_buf, advance m_pos
    │
    ├── len ≥ 8KB? → drain buffer + inner.read(buf, len) suspension
    │
    └── buffer empty → inner.read(m_buf, 8KB) suspension → memcpy to output

Internal state:
  m_buf[8KB]    stack-allocated buffer
  m_pos         next unread byte in buffer
  m_filled      total bytes in buffer (0 after refill)
```

The first `read()` call fills the buffer from the inner reader. Subsequent small reads drain it with zero additional I/O — `memcpy` only. When the buffer is exhausted, it refills from the inner reader.

Large reads (≥ 8KB) skip the buffer: any pending buffered data is drained first, then the remaining bytes are read directly from the inner reader. This avoids a double-copy for bulk transfers.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `BufReader(R reader)` | | Take ownership of `reader`. Move-only |
| `read(buf, len)` | `Promise<ssize_t>` | Buffered read. Resolves to bytes read (0 = EOF) |
| `inner()` | `R&` | Access the inner reader (e.g., for `close()`) |

## Usage Examples

### Byte-at-a-time parsing — `.await()`

```cpp
xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));

// Read 4-byte header
char header[4];
buf.read(header, 4).await();

// Read payload length from header
uint32_t len = ntohl(*reinterpret_cast<uint32_t *>(header));

// Read payload (may be large — bypasses buffer)
auto payload = std::make_shared<std::vector<char>>(len);
buf.read(payload->data(), len).await();
```

### Byte-at-a-time parsing — `co_await` (C++20)

```cpp
xpp::Promise<void> parse_frame(xpp::net::TcpStream conn) {
    xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));
    char header[4];
    co_await buf.read(header, 4);
    uint32_t len = ntohl(*reinterpret_cast<uint32_t *>(header));
    auto payload = std::make_shared<std::vector<char>>(len);
    co_await buf.read(payload->data(), len);
}
```

### Compose with io::read_all — `.await()`

```cpp
xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));
std::string method;
char c;
while (true) {
    buf.read(&c, 1).await();
    if (c == ' ') break;
    method += c;
}
auto body = xpp::io::read_all(buf).await();
```

### Compose with io::read_all — `co_await` (C++20)

```cpp
xpp::Promise<void> read_request(xpp::net::TcpStream conn) {
    xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));
    std::string method; char c;
    while (true) {
        co_await buf.read(&c, 1);
        if (c == ' ') break; method += c;
    }
    auto body = co_await xpp::io::read_all(buf);
}
```

### Large bypass

Reads ≥ 8KB go directly to the inner reader. No double-buffering:

```cpp
// .await()
xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));
char data[65536];  // 64KB — larger than buffer
buf.read(data, sizeof(data)).await();  // bypasses buffer entirely

// co_await (C++20)
xpp::Promise<void> download_chunk(xpp::net::TcpStream conn) {
    xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));
    char data[65536];
    co_await buf.read(data, sizeof(data));
}
```
