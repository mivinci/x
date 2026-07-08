# Sink

## Introduction

`xpp::io::Sink` is a write-discarding async writer. Every call to `write()` returns `len` immediately (the data is silently discarded). Useful for benchmarks, `/dev/null` equivalents, and situations where a writer is expected but the output is not needed.

Satisfies the `AsyncWriter` concept — composable with `io::copy` and `BufWriter`. C++11-compatible (uses `xpp::resolve(len)`).

## Example — `.await()`

```cpp
#include <xpp/io/sink.h>

xpp::io::Sink s;
ssize_t n = s.write("hello", 5).await();
// n == 5 (data discarded)
```

## How it works

`Sink` is a simple struct whose `write()` returns `xpp::resolve(static_cast<ssize_t>(len))` — an immediately-resolved Promise reporting that all bytes were "written". No I/O, no state, no coroutine overhead.

Use the `sink()` factory function:

```cpp
auto s = xpp::io::sink();
```

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `sink()` | `Sink` | Factory returning a write-discarding writer |

## Usage Examples

### Benchmark copy throughput — `.await()`

```cpp
auto s = xpp::io::sink();
xpp::io::copy(reader, s).await();  // measures pure read throughput
```

### Benchmark copy throughput — `co_await` (C++20)

```cpp
xpp::Promise<void> benchmark_copy(xpp::io::AsyncReader auto &reader) {
    auto s = xpp::io::sink();
    co_await xpp::io::copy(reader, s);
}
```

### Discard unwanted output — `.await()`

```cpp
char header[4];
conn.read(header, 4).await();

auto s = xpp::io::sink();
xpp::io::copy(conn, s).await();  // read and discard remaining data
```

### Discard unwanted output — `co_await` (C++20)

```cpp
xpp::Promise<void> parse_request(xpp::net::TcpStream conn) {
    char header[4];
    co_await conn.read(header, 4);
    auto s = xpp::io::sink();
    co_await xpp::io::copy(conn, s);
}
```

### As default writer parameter

```cpp
// .await()
auto s = xpp::io::sink();
xpp::io::copy(reader, s).await();

// co_await (C++20)
template <AsyncWriter W>
xpp::Promise<void> log_or_discard(xpp::io::AsyncReader auto &reader, W &writer) {
    co_await xpp::io::copy(reader, writer);
}
auto s = xpp::io::sink();
log_or_discard(reader, s).await();
```
