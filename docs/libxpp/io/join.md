# Join

## Introduction

`xpp::io::join(reader, writer)` combines an `AsyncReader` and `AsyncWriter` into a single type that satisfies both concepts. Useful when you have separate read/write halves (e.g., from `simplex`) and need a single bidirectional handle.

C++11-compatible. Duck-typed — the combined type automatically satisfies both `AsyncReader` and `AsyncWriter` concepts.

## Example — `.await()`

```cpp
#include <xpp/io/join.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [reader, writer] = xpp::io::simplex(256);
auto joined = xpp::io::join(std::move(reader), std::move(writer));

// Now reads from reader and writes to writer through a single object
joined.write("hello", 5).await();
char buf[8];
joined.read(buf, 8).await();  // buf = "hello"
```

## Example — `co_await` (C++20)

```cpp
auto [reader, writer] = xpp::io::simplex(256);
auto joined = xpp::io::join(std::move(reader), std::move(writer));

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

### Combine simplex halves — `.await()`

```cpp
auto [reader, writer] = xpp::io::simplex(256);
auto conn = xpp::io::join(std::move(reader), std::move(writer));

conn.write("ping", 4).await();
char buf[8];
conn.read(buf, 8).await();  // reads "ping" from self
```

### Combine simplex halves — `co_await` (C++20)

```cpp
auto [reader, writer] = xpp::io::simplex(256);
auto conn = xpp::io::join(std::move(reader), std::move(writer));

co_await conn.write("ping", 4);
char buf[8];
co_await conn.read(buf, 8);
```

### Separate read/write TcpStreams — `.await()`

```cpp
auto reader = popen_tcp_stream.read();
auto writer = popen_tcp_stream.write();
auto conn = xpp::io::join(reader, writer);

xpp::io::copy(conn, file).await();
```

### Separate read/write TcpStreams — `co_await` (C++20)

```cpp
auto reader = popen_tcp_stream.read();
auto writer = popen_tcp_stream.write();
auto conn = xpp::io::join(reader, writer);

co_await xpp::io::copy(conn, file);
```
