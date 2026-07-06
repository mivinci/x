# BufReader

## Introduction

`xpp::io::BufReader<R>` wraps any `AsyncReader` with an internal 8KB buffer. Small reads copy from the buffer with zero I/O overhead; large reads (≥ 8KB) bypass the buffer entirely. Reduces per-call Promise overhead for byte-at-a-time parsing over TCP.

Satisfies the `AsyncReader` concept — composable with `io::read_all`, `io::copy`, or nested in another `BufReader`. Takes ownership of the inner reader via move semantics. Coroutine (C++20) or struct+move fallback (C++11).

```cpp
#include <xpp/io/buf_reader.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto conn = xpp::net::TcpStream::connect("127.0.0.1:9090").wait().unwrap();
xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));

// Small reads: copy from buffer, zero per-call Promise overhead
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
    ├── len ≥ 8KB? → drain buffer + co_await inner.read(buf, len)
    │
    └── buffer empty → co_await inner.read(m_buf, 8KB) → memcpy to output

Internal state:
  m_buf[8KB]    stack-allocated buffer
  m_pos         next unread byte in buffer
  m_filled      total bytes in buffer (0 after refill)
```

The first `read()` call fills the buffer from the inner reader. Subsequent small reads drain it with zero additional I/O — `memcpy` only, no `co_await` overhead. When the buffer is exhausted, it refills from the inner reader.

Large reads (≥ 8KB) skip the buffer: any pending buffered data is drained first, then the remaining bytes are read directly from the inner reader. This avoids a double-copy for bulk transfers.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `BufReader(R reader)` | | Take ownership of `reader`. Move-only |
| `read(buf, len)` | `Promise<ssize_t>` | Buffered read. Resolves to bytes read (0 = EOF) |
| `inner()` | `R&` | Access the inner reader (e.g., for `close()`) |

## Usage Examples

### Byte-at-a-time parsing

```cpp
xpp::Promise<void> parse_frame(xpp::net::TcpStream conn) {
    xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));

    // Read 4-byte header
    char header[4];
    co_await buf.read(header, 4);

    // Read payload length from header
    uint32_t len = ntohl(*reinterpret_cast<uint32_t *>(header));

    // Read payload (may be large — bypasses buffer)
    auto payload = std::make_shared<std::vector<char>>(len);
    co_await buf.read(payload->data(), len);
}
```

### Compose with io::read_all

```cpp
xpp::Promise<void> read_request(xpp::net::TcpStream conn) {
    xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));

    // Parses HTTP request line byte-by-byte
    std::string method;
    char c;
    while (true) {
        co_await buf.read(&c, 1);
        if (c == ' ') break;
        method += c;
    }

    // Read remaining body with io::read_all
    auto body = co_await xpp::io::read_all(buf);
}
```

### Large bypass

Reads ≥ 8KB go directly to the inner reader. No double-buffering:

```cpp
xpp::Promise<void> download_chunk(xpp::net::TcpStream conn) {
    xpp::io::BufReader<xpp::net::TcpStream> buf(std::move(conn));

    char data[65536];  // 64KB — larger than buffer
    co_await buf.read(data, sizeof(data));  // bypasses buffer entirely
}
```

## Examples

All examples above show coroutine usage (C++20). For C++11, `BufReader` works identically — the `.read()` method uses struct+move instead of `co_await` internally. See [Utilities](util.md) for composing `BufReader` with `io::read_all` and `io::copy`.
