# I/O Utilities

## Introduction

`xpp::io::read_all` and `xpp::io::copy` are template utility functions that work on any type with `read(void*, size_t) → Promise<ssize_t>` and `write(const void*, size_t) → Promise<ssize_t>` — duck-typed, no traits or inheritance required. Both use coroutine loops with an 8KB stack buffer (matching Rust's `DEFAULT_BUF_SIZE`).

C++20 only (`co_await`).

```cpp
#include <xpp/io/util.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

// Copy from TcpStream to File
auto stream = xpp::net::TcpStream::connect("127.0.0.1", 9090).wait().unwrap();
auto file = xpp::fs::File::create("output.bin").wait();

xpp::io::copy(stream, file).wait();
```

## API Reference

### read_all

```cpp
template <class R>
Promise<std::vector<uint8_t>> read_all(R &reader);
```

Reads the entire byte stream into a vector. Stops when `read` returns ≤ 0 (EOF or error). Uses an 8KB stack buffer — zero heap allocation for the buffer.

The reader must have `read(void*, size_t) → Promise<ssize_t>`. Compatible with `TcpStream`, `fs::File` (cursor mode), and any user-defined type matching the signature.

### copy

```cpp
template <class R, class W>
Promise<void> copy(R &reader, W &writer);
```

Pipes all content from reader to writer. Uses an 8KB stack buffer.

The reader must have `read(void*, size_t) → Promise<ssize_t>`. The writer must have `write(const void*, size_t) → Promise<ssize_t>`.

## Usage Examples

### Read entire response from TcpStream

```cpp
xpp::Promise<void> fetch() {
    auto stream = co_await xpp::net::TcpStream::connect("example.com", 80);
    co_await stream.write("GET / HTTP/1.0\r\n\r\n", 18);
    auto data = co_await xpp::io::read_all(stream);
    printf("%.*s\n", (int)data.size(), data.data());
}
```

### Copy from TcpStream to File

```cpp
xpp::Promise<void> download() {
    auto stream = co_await xpp::net::TcpStream::connect("example.com", 80);
    co_await stream.write("GET / HTTP/1.0\r\n\r\n", 18);

    auto file = co_await xpp::fs::File::create("response.txt");
    co_await xpp::io::copy(stream, file);
}
```

### Read from File cursor

```cpp
xpp::Promise<void> process() {
    auto file = co_await xpp::fs::File::open("data.txt");
    char buf[16];
    ssize_t n = co_await file.read(buf, sizeof(buf));  // cursor advance
    // n == 16, cursor at 16

    auto rest = co_await xpp::io::read_all(file);  // reads from cursor to EOF
}
```
