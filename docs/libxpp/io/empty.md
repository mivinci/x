# Empty

## Introduction

`xpp::io::Empty` is an always-EOF async reader. Every call to `read()` returns 0 immediately. Useful for testing, placeholder values, and situations where a reader is expected but no data is available.

Satisfies the `AsyncReader` concept — composable with `io::read_all`, `io::copy`, and `BufReader`. C++11-compatible (no coroutines needed — uses `xpp::resolve(0)`).

```cpp
#include <xpp/io/empty.h>

xpp::io::Empty e;
ssize_t n = e.read(nullptr, 10).await();
// n == 0
```

## How it works

`Empty` is a simple struct whose `read()` returns `xpp::resolve(0)` — an immediately-resolved Promise returning 0 bytes. No I/O, no state, no coroutine overhead.

Use the `empty()` factory function for a cleaner API:

```cpp
auto e = xpp::io::empty();
```

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `empty()` | `Empty` | Factory returning an always-EOF reader |

## Usage Examples

### As default reader parameter

```cpp
template <AsyncReader R>
xpp::Promise<void> process(R &reader) {
    auto data = co_await xpp::io::read_all(reader);
    // process data...
}

// Call with empty — nothing to read
auto e = xpp::io::empty();
process(e).await();  // data is empty vector
```

### Combined with Take

```cpp
xpp::io::Take<xpp::io::Empty> limit(xpp::io::empty(), 0);
ssize_t n = limit.read(nullptr, 10).await();
// n == 0
```
