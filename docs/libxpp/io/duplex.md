# Duplex

## Introduction

`xpp::io::duplex(size)` creates a pair of connected `DuplexStream`s backed by two internal ring buffers. Each half satisfies both `AsyncReader` and `AsyncWriter` — writing to one side makes data readable on the other. Like tokio's `DuplexStream` or Go's `net.Pipe`.

Works with `.await()`, `co_await` (C++20 coroutines), or `.then()` chains (C++11). Single-threaded — no atomics or mutex.

## Example — `.await()`

```cpp
#include <xpp/io/duplex.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [a, b] = xpp::io::duplex(4096);

// a → b and b → a simultaneously
a.write("ping", 4).await();
char buf[8];
b.read(buf, 8).await();  // buf = "ping"
```

## Example — `co_await` (C++20)

```cpp
auto [a, b] = xpp::io::duplex(4096);
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

### Bidirectional echo — `.await()`

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [client, server] = xpp::io::duplex(256);

client.write("ping", 4).await();
char buf[8];
server.read(buf, 8).await();        // server reads "ping"
server.write("pong", 4).await();    // server responds
client.read(buf, 8).await();        // client reads "pong"
```

### Bidirectional echo — `co_await` (C++20)

```cpp
auto [client, server] = xpp::io::duplex(256);
co_await client.write("ping", 4);
char buf[8];
co_await server.read(buf, 8);       // server reads "ping"
co_await server.write("pong", 4);   // server responds
co_await client.read(buf, 8);       // client reads "pong"
```

### Test I/O utilities without TCP — `.await()`

```cpp
auto [a, b] = xpp::io::duplex(4096);

// Producer
b.write("hello world", 11).await();
b.close();

// Consumer with BufReader
xpp::io::BufReader buf{std::move(a)};
auto data = xpp::io::read_all(buf).await();
// data == "hello world"
```

### Test I/O utilities without TCP — `co_await` (C++20)

```cpp
auto [a, b] = xpp::io::duplex(4096);

auto wp = [](xpp::io::DuplexStream &w) -> xpp::Promise<void> {
    co_await w.write("hello world", 11);
    w.close();
};
auto rp = [](xpp::io::DuplexStream &r) -> xpp::Promise<void> {
    xpp::io::BufReader buf{std::move(r)};
    auto data = co_await xpp::io::read_all(buf);
};
```
