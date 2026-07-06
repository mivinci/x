# BufWriter

## Introduction

`xpp::io::BufWriter<W>` wraps any `AsyncWriter` with an internal 8KB buffer. Small writes fill the buffer; when it's full, pending data is automatically flushed to the inner writer. Large writes (≥ 8KB) bypass the buffer entirely. Reduces per-call syscall overhead for message-building over TCP.

Satisfies the `AsyncWriter` concept — composable with `io::copy`. Takes ownership of the inner writer via move semantics. C++20 coroutine-only.

**IMPORTANT**: `flush()` must be called explicitly before dropping. The destructor does **NOT** flush un-sent data — matching Rust's BufWriter behavior.

```cpp
#include <xpp/io/buf_writer.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto conn = xpp::net::TcpStream::connect("127.0.0.1:9090").wait().unwrap();
xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));

// Small writes accumulate — no syscalls yet
co_await buf.write("HTTP/1.0 200 OK\r\n", 17);
co_await buf.write("Content-Length: 5\r\n\r\n", 22);
co_await buf.write("hello", 5);

// Must flush before dropping!
co_await buf.flush();
```

## How it works

```text
write(buf, len)
    │
    ├── len ≥ 8KB? → flush pending + co_await inner.write(buf, len)
    │
    ├── buffer would overflow? → flush pending first
    │
    └── memcpy to m_buf, advance m_pos

flush()
    └── m_pos > 0? → co_await inner.write(m_buf, m_pos) → m_pos = 0

~BufWriter()
    └── does NOT flush (Rust behavior)
```

Small writes accumulate in the internal buffer with zero I/O. When the buffer would overflow, pending data is automatically flushed first, then the new data is buffered. Large writes (≥ 8KB) flush any pending data, then write directly to the inner writer — avoiding double-buffering.

The destructor does NOT call `flush()`. This matches Rust's behavior: the caller is responsible for flushing. If `flush()` is forgotten, data is silently discarded. This avoids the impossible choice of blocking in a destructor or losing data silently — the convention is clear.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `BufWriter(W writer)` | | Take ownership of `writer`. Move-only |
| `write(buf, len)` | `Promise<ssize_t>` | Buffered write. May trigger auto-flush |
| `flush()` | `Promise<void>` | Send all buffered data to inner writer |
| `inner()` | `W&` | Access the inner writer (e.g., for `close()`) |

## Usage Examples

### Build HTTP response

```cpp
xpp::Promise<void> respond(xpp::net::TcpStream conn) {
    xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));

    co_await buf.write("HTTP/1.0 200 OK\r\n", 17);
    co_await buf.write("Content-Type: text/plain\r\n", 26);
    co_await buf.write("Content-Length: 5\r\n", 20);
    co_await buf.write("\r\n", 2);
    co_await buf.write("hello", 5);

    // Must flush — bytes are still in buffer
    co_await buf.flush();
}
```

### Large bypass

Writes ≥ 8KB go directly to the inner writer:

```cpp
xpp::Promise<void> upload(xpp::net::TcpStream conn, const void *data, size_t len) {
    xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));

    // Small header — buffered
    co_await buf.write("data: ", 6);

    // Large body — bypasses buffer
    co_await buf.write(data, len);

    co_await buf.flush();
}
```

### Close after flush

After flushing, use `inner()` to close the connection:

```cpp
co_await buf.flush();
buf.inner().close();  // close the underlying TcpStream
```

## Coroutine Examples

`BufWriter` is itself coroutine-based — all examples above are valid coroutine usage. See [Utilities](util.md) for how to compose `BufWriter` with `io::copy`.
