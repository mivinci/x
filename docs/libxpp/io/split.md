# Split

## Introduction

`xpp::io::split(stream)` splits any type satisfying both `AsyncReader` and `AsyncWriter` into independent `ReadHalf<T>` and `WriteHalf<T>`. Like tokio's `io::split()` — the two halves share the underlying stream via `Arc<T>` (atomic refcount).

## Example — `.await()`

```cpp
#include <xpp/io/split.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [reader, writer] = xpp::io::split(std::move(stream));

// Concurrent read + write on the same connection
xpp::all(reader.read(buf, 10), writer.write("hi", 2)).await();
```

## Example — `co_await` (C++20)

```cpp
auto [reader, writer] = xpp::io::split(std::move(stream));
auto [n, _] = co_await xpp::all(reader.read(buf, 10), writer.write("hi", 2));
```

## How it works

```
io::split(stream)
    └── Arc<T>::make(std::move(stream))
        ├── ReadHalf<T> { Arc<T> m_inner }  → read(buf, len)
        └── WriteHalf<T> { Arc<T> m_inner } → write(buf, len), flush()

Both halves hold a copy of the Arc<T> refcount.
The underlying stream is destroyed when the last half drops.
```

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `split(stream)` | `pair<ReadHalf<T>, WriteHalf<T>>` | Split into independent read/write halves |

## Usage Examples

### Split TcpStream — `.await()`

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [reader, writer] = xpp::io::split(std::move(conn));
char buf[16];

// Read first
ssize_t n = reader.read(buf, sizeof(buf)).await();

// Then echo back
writer.write(buf, static_cast<size_t>(n)).await();
```

### Split TcpStream — `co_await` (C++20)

```cpp
xpp::Promise<void> echo(xpp::net::TcpStream conn) {
    auto [reader, writer] = xpp::io::split(std::move(conn));
    char buf[16];
    ssize_t n = co_await reader.read(buf, sizeof(buf));
    co_await writer.write(buf, static_cast<size_t>(n));
}
```
