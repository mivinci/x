# Duplex

## Introduction

`xpp::io::duplex(size)` creates a pair of connected `DuplexStream`s backed by two internal ring buffers. Each half satisfies both `AsyncReader` and `AsyncWriter` — writing to one side makes data readable on the other. Like tokio's `DuplexStream` or Go's `net.Pipe`.

Coroutine-only (C++20). Single-threaded — no atomics or mutex.

```cpp
#include <xpp/io/duplex.h>

auto [a, b] = xpp::io::duplex(4096);

// a → b and b → a simultaneously
co_await a.write("ping", 4);
char buf[8];
co_await b.read(buf, 8);  // buf = "ping"
```

## How it works

```text
duplex(4096)
├── side[0]: A's receive buffer (B writes here, A reads)
├── side[1]: B's receive buffer (A writes here, B reads)
├── DuplexStream(idx=0) → reads from side[0], writes to side[1]
└── DuplexStream(idx=1) → reads from side[1], writes to side[0]

write(buf, len):
  copy to opposite side's buffer → wake opposite reader
read(buf, len):
  copy from own buffer → wake opposite writer
close():
  set opposite side's closed flag → wake opposite reader (EOF)
```

Writing suspends when the destination buffer is full; reading suspends when the source buffer is empty. Both use `PromiseResolver` for wakeup — no event loop needed, resolution is synchronous from the opposite half.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `duplex(size)` | `pair<DuplexStream, DuplexStream>` | Create connected pair with `size`-byte ring buffers per direction |
| `DuplexStream::read(buf, len)` | `Promise<ssize_t>` | Read from this side. Suspends when empty, returns 0 on EOF |
| `DuplexStream::write(buf, len)` | `Promise<ssize_t>` | Write to the other side. Suspends when buffer full |
| `DuplexStream::flush()` | `Promise<void>` | No-op (data flows directly to buffer) |
| `DuplexStream::close()` | `void` | Close write direction. Other side sees EOF on read |

## Usage Examples

### Bidirectional echo

```cpp
auto [client, server] = xpp::io::duplex(256);

co_await client.write("ping", 4);
char buf[8];
co_await server.read(buf, 8);       // server reads "ping"
co_await server.write("pong", 4);   // server responds
co_await client.read(buf, 8);       // client reads "pong"
```

### Test I/O utilities without TCP

```cpp
auto [a, b] = xpp::io::duplex(4096);

// Producer
auto wp = [](xpp::io::DuplexStream &w) -> xpp::Promise<void> {
    co_await w.write("hello world", 11);
    w.close();
};

// Consumer with BufReader
auto rp = [](xpp::io::DuplexStream &r) -> xpp::Promise<void> {
    xpp::io::BufReader buf{std::move(r)};
    auto data = co_await xpp::io::read_all(buf);
    // data == "hello world"
};
```
