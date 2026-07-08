# BufWriter

## Introduction

`xpp::io::BufWriter<W>` wraps any `AsyncWriter` with an internal 8KB buffer. Small writes fill the buffer; when it's full, pending data is automatically flushed to the inner writer. Large writes (≥ 8KB) bypass the buffer entirely. Reduces per-call syscall overhead for message-building over TCP.

Satisfies the `AsyncWriter` concept — composable with `io::copy`. Takes ownership of the inner writer via move semantics. Works with `.await()`, `co_await` (C++20), or `.then()` chains (C++11).

**IMPORTANT**: `flush()` must be called explicitly before dropping. The destructor does **NOT** flush un-sent data — matching Rust's BufWriter behavior.

## Example — `.await()`

```cpp
#include <xpp/io/buf_writer.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto conn = xpp::net::TcpStream::connect("127.0.0.1:9090").await().unwrap();
xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));

// Small writes accumulate — no syscalls yet
buf.write("HTTP/1.0 200 OK\r\n", 17).await();
buf.write("Content-Length: 5\r\n\r\n", 22).await();
buf.write("hello", 5).await();

// Must flush before dropping!
buf.flush().await();
```

## Example — `co_await` (C++20)

```cpp
auto conn = xpp::net::TcpStream::connect("127.0.0.1:9090").await().unwrap();
xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));

co_await buf.write("HTTP/1.0 200 OK\r\n", 17);
co_await buf.write("Content-Length: 5\r\n\r\n", 22);
co_await buf.write("hello", 5);
co_await buf.flush();
```

## How it works

```text
write(buf, len)
    │
    ├── len ≥ 8KB? → flush pending + inner.write(buf, len) suspension
    │
    ├── buffer would overflow? → flush pending first
    │
    └── memcpy to m_buf, advance m_pos

flush()
    └── m_pos > 0? → inner.write(m_buf, m_pos) suspension → m_pos = 0

~BufWriter()
    └── does NOT flush (Rust behavior)
```

Small writes accumulate in the internal buffer with zero I/O. When the buffer would overflow, pending data is automatically flushed first, then the new data is buffered. Large writes (≥ 8KB) flush any pending data, then write directly to the inner writer — avoiding double-buffering.

The destructor does NOT call `flush()`. This matches Rust's behavior: the caller is responsible for flushing. If `flush()` is forgotten, data is silently discarded.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `BufWriter(W writer)` | | Take ownership of `writer`. Move-only |
| `write(buf, len)` | `Promise<ssize_t>` | Buffered write. May trigger auto-flush |
| `flush()` | `Promise<void>` | Send all buffered data to inner writer |
| `inner()` | `W&` | Access the inner writer (e.g., for `close()`) |

## Usage Examples

### Build HTTP response — `.await()`

```cpp
xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));

buf.write("HTTP/1.0 200 OK\r\n", 17).await();
buf.write("Content-Type: text/plain\r\n", 26).await();
buf.write("Content-Length: 5\r\n", 20).await();
buf.write("\r\n", 2).await();
buf.write("hello", 5).await();
buf.flush().await();  // Must flush — bytes still in buffer
```

### Build HTTP response — `co_await` (C++20)

```cpp
xpp::Promise<void> respond(xpp::net::TcpStream conn) {
    xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));
    co_await buf.write("HTTP/1.0 200 OK\r\n", 17);
    co_await buf.write("Content-Type: text/plain\r\n", 26);
    co_await buf.write("Content-Length: 5\r\n", 20);
    co_await buf.write("\r\n", 2);
    co_await buf.write("hello", 5);
    co_await buf.flush();
}
```

### Large bypass — `.await()`

```cpp
xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));

// Small header — buffered
buf.write("data: ", 6).await();

// Large body — bypasses buffer
buf.write(data, len).await();  // writes ≥ 8KB go directly to inner writer
buf.flush().await();
```

### Large bypass — `co_await` (C++20)

```cpp
xpp::Promise<void> upload(xpp::net::TcpStream conn, const void *data, size_t len) {
    xpp::io::BufWriter<xpp::net::TcpStream> buf(std::move(conn));
    co_await buf.write("data: ", 6);
    co_await buf.write(data, len);
    co_await buf.flush();
}
```

### Close after flush

```cpp
// .await()
buf.flush().await();
buf.inner().close();  // close the underlying TcpStream

// co_await (C++20)
co_await buf.flush();
buf.inner().close();
```
