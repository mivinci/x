# Join

## Introduction

`xpp::io::join(reader, writer)` combines an `AsyncReader` and `AsyncWriter` into a single type that satisfies both concepts. Useful when you have separate read/write halves (e.g., from `simplex`) and need a single bidirectional handle.

C++11-compatible (no coroutines). Duck-typed — the combined type automatically satisfies both `AsyncReader` and `AsyncWriter` concepts.

```cpp
#include <xpp/io/join.h>

auto [reader, writer] = xpp::io::simplex(256);
auto joined = xpp::io::join(std::move(reader), std::move(writer));

// Now reads from reader and writes to writer through a single object
co_await joined.write("hello", 5);
char buf[8];
co_await joined.read(buf, 8);  // buf = "hello"
```

## How it works

`Join<R, W>` stores the reader and writer by value (takes ownership). `read()` delegates to the inner reader; `write()` delegates to the inner writer; `flush()` and `close()` delegate to the inner writer. Zero overhead — all calls are inlined through the template.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `join(reader, writer)` | `Join<R, W>` | Combine reader and writer into one type |
| `Join::read(buf, len)` | `Promise<ssize_t>` | Forward to inner reader |
| `Join::write(buf, len)` | `Promise<ssize_t>` | Forward to inner writer |
| `Join::flush()` | `Promise<void>` | Forward to inner writer |
| `Join::close()` | `void` | Forward to inner writer |

## Usage Examples

### Combine simplex halves

```cpp
auto [reader, writer] = xpp::io::simplex(256);
auto conn = xpp::io::join(std::move(reader), std::move(writer));

co_await conn.write("ping", 4);

char buf[8];
co_await conn.read(buf, 8);  // reads "ping" from self
```

### Separate read/write TcpStreams

```cpp
auto reader = popen_tcp_stream.read();
auto writer = popen_tcp_stream.write();
auto conn = xpp::io::join(reader, writer);

// Now treated as a single stream by io utilities
co_await xpp::io::copy(conn, file);
```
