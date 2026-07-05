# Simplex

## Introduction

`xpp::io::simplex(size)` creates a unidirectional pipe — a `SimplexReader` and `SimplexWriter` sharing a single ring buffer. Simpler than `duplex()` which provides two buffers for bidirectional communication. Like Go's `io.Pipe`.

Coroutine-only (C++20). Single-threaded.

```cpp
#include <xpp/io/simplex.h>

auto [reader, writer] = xpp::io::simplex(4096);

co_await writer.write("hello", 5);
writer.close();

char buf[8];
co_await reader.read(buf, 8);   // buf = "hello"
co_await reader.read(buf, 8);   // returns 0 (EOF)
```

## How it works

```text
simplex(4096)
├── ring buffer (4096 bytes)
├── SimplexReader → reads from buffer, returns 0 on EOF
└── SimplexWriter → writes to buffer, close() signals EOF

write(buf, len):
  copy to buffer → wake reader
read(buf, len):
  copy from buffer → wake writer
close():
  set closed flag → wake reader (EOF)
```

Writing suspends when the buffer is full; reading suspends when the buffer is empty.

## API Reference

| Method | Returns | Description |
| ------ | ------- | ----------- |
| `simplex(size)` | `pair<SimplexReader, SimplexWriter>` | Create unidirectional pipe with `size`-byte buffer |
| `SimplexReader::read(buf, len)` | `Promise<ssize_t>` | Read from pipe. Suspends when empty, 0 on EOF |
| `SimplexWriter::write(buf, len)` | `Promise<ssize_t>` | Write to pipe. Suspends when buffer full |
| `SimplexWriter::flush()` | `Promise<void>` | No-op |
| `SimplexWriter::close()` | `void` | Signal EOF to reader |

## Usage Examples

### Testing read_all

```cpp
auto [reader, writer] = xpp::io::simplex(256);

co_await writer.write("hello world", 11);
writer.close();

auto data = co_await xpp::io::read_all(reader);
// data.size() == 11, data == "hello world"
```

### Testing copy

```cpp
auto [reader, writer] = xpp::io::simplex(256);

co_await writer.write("ping", 4);
writer.close();

xpp::io::Empty dst;
co_await xpp::io::copy(reader, dst);  // reads "ping" and discards
```
