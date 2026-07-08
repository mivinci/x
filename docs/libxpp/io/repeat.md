# Repeat

## Introduction

`xpp::io::Repeat` is an infinite repeating byte reader. Every call to `read()` fills the buffer with the same byte and returns `len`. Never returns EOF — useful for benchmarks and generating padding data.

C++11-compatible (no coroutines). Satisfies the `AsyncReader` concept.

## Example — `.await()`

```cpp
#include <xpp/io/repeat.h>

auto r = xpp::io::repeat('A');
char buf[16];
ssize_t n = r.read(buf, sizeof(buf)).await();
// n == 16, buf = "AAAAAAAAAAAAAAAA"
```

## How it works

`Repeat` stores a single byte and returns `xpp::resolve(len)` after filling the buffer with `std::memset`. No I/O, no state changes, no coroutine overhead.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `repeat(byte = 0)` | `Repeat` | Create a repeat reader for the given byte |

## Usage Examples

### Benchmark throughput — `.await()`

```cpp
auto r = xpp::io::repeat();
auto s = xpp::io::sink();
xpp::io::copy(r, s).await();  // measures pure reader + copy throughput
```

### Benchmark throughput — `co_await` (C++20)

```cpp
auto r = xpp::io::repeat();
auto s = xpp::io::sink();
co_await xpp::io::copy(r, s);
```

### Padding generation

```cpp
// .await()
auto zeros = xpp::io::repeat(0);
char pad[1024];
zeros.read(pad, 1024).await();  // 1KB of zeros

// co_await (C++20)
auto zeros = xpp::io::repeat(0);
char pad[1024];
co_await zeros.read(pad, 1024);
```
