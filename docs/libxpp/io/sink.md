# Sink

## Introduction

`xpp::io::Sink` is a write-discarding async writer. Every call to `write()` returns `len` immediately (the data is silently discarded). Useful for benchmarks, `/dev/null` equivalents, and situations where a writer is expected but the output is not needed.

Satisfies the `AsyncWriter` concept — composable with `io::copy` and `BufWriter`. C++11-compatible (no coroutines needed — uses `xpp::resolve(len)`).

```cpp
#include <xpp/io/sink.h>

xpp::io::Sink s;
ssize_t n = s.write("hello", 5).await();
// n == 5 (data discarded)
```

## How it works

`Sink` is a simple struct whose `write()` returns `xpp::resolve(static_cast<ssize_t>(len))` — an immediately-resolved Promise reporting that all bytes were "written". No I/O, no state, no coroutine overhead.

Use the `sink()` factory function for a cleaner API:

```cpp
auto s = xpp::io::sink();
```

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `sink()` | `Sink` | Factory returning a write-discarding writer |

## Usage Examples

### Benchmark copy throughput

```cpp
xpp::Promise<void> benchmark_copy(xpp::io::AsyncReader auto &reader) {
    auto s = xpp::io::sink();
    auto start = xpp::after(0);  // immediate resolve

    // Copy from reader to /dev/null — measures pure read throughput
    co_await xpp::io::copy(reader, s);
}
```

### Discard unwanted output

```cpp
xpp::Promise<void> parse_request(xpp::net::TcpStream conn) {
    // Read 4-byte header, discard the rest
    char header[4];
    co_await conn.read(header, 4);

    auto s = xpp::io::sink();
    co_await xpp::io::copy(conn, s);  // read and discard remaining data
}
```

### As default writer parameter

```cpp
template <AsyncWriter W>
xpp::Promise<void> log_or_discard(xpp::io::AsyncReader auto &reader, W &writer) {
    co_await xpp::io::copy(reader, writer);
}

// Call with sink — output is discarded
auto s = xpp::io::sink();
log_or_discard(reader, s).await();
```
