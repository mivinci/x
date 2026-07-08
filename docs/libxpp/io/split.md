# Split

## Introduction

`xpp::io::split(stream)` splits any type satisfying both `AsyncReader` and `AsyncWriter` into independent `ReadHalf<T>` and `WriteHalf<T>`. Like tokio's `io::split()` — the two halves share the underlying stream via `Shared<T>` (Rc or Arc depending on `XPP_MT`).

```cpp
#include <xpp/io/split.h>

auto [reader, writer] = xpp::io::split(std::move(stream));

// Concurrent read + write on the same connection
xpp::all(reader.read(buf, 10), writer.write("hi", 2)).await();
```

## How it works

```
io::split(stream)
    └── Shared<T>::make(std::move(stream))
        ├── ReadHalf<T> { Shared<T> m_inner }  → read(buf, len)
        └── WriteHalf<T> { Shared<T> m_inner } → write(buf, len), flush()

Both halves hold a copy of the Shared<T> refcount.
The underlying stream is destroyed when the last half drops.
```

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `split(stream)` | `pair<ReadHalf<T>, WriteHalf<T>>` | Split into independent read/write halves |

## Usage Examples

### Split TcpStream

```cpp
xpp::Promise<void> echo(xpp::net::TcpStream conn) {
    auto [reader, writer] = xpp::io::split(std::move(conn));

    char buf[16];
    auto rp = reader.read(buf, sizeof(buf));
    auto wp = writer.write(buf, sizeof(buf));  // wrong: buf not filled yet

    co_await xpp::all(std::move(rp), std::move(wp));
}
```
