# I/O Utilities

## Introduction

`xpp::io::read_all` and `xpp::io::copy` are template utility functions that work on any type with `read(void*, size_t) → Promise<ssize_t>` and `write(const void*, size_t) → Promise<ssize_t>` — duck-typed, no traits or inheritance required. Both use an 8KB buffer (matching Rust's `DEFAULT_BUF_SIZE`).

C++20: coroutine loops with stack buffers. C++11 + `XPP_FIBER`: fiber `.await()` loops. C++11 without fiber: struct+move fallback with `xpp::Shared` heap buffer.

## Example — `.await()`

```cpp
#include <xpp/io/utils.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

// Copy from TcpStream to File
auto stream = xpp::net::TcpStream::connect("127.0.0.1:9090").await().unwrap();
auto file = xpp::fs::File::create("output.bin").await();

xpp::io::copy(stream, file).await();
```

## Example — `co_await` (C++20)

```cpp
auto stream = co_await xpp::net::TcpStream::connect("127.0.0.1:9090");
auto file = co_await xpp::fs::File::create("output.bin");
co_await xpp::io::copy(stream, file);
```

## API Reference

### read_all

```cpp
template <class R>
Promise<std::vector<uint8_t>> read_all(R &reader);
```

Reads the entire byte stream into a vector. Stops when `read` returns ≤ 0 (EOF or error). Uses an 8KB buffer.

The reader must have `read(void*, size_t) → Promise<ssize_t>`. Compatible with `TcpStream`, `fs::File` (cursor mode), and any user-defined type matching the signature.

### copy

```cpp
template <class R, class W>
Promise<void> copy(R &reader, W &writer);
```

Pipes all content from reader to writer. Uses an 8KB buffer.

The reader must have `read(void*, size_t) → Promise<ssize_t>`. The writer must have `write(const void*, size_t) → Promise<ssize_t>`.

## Usage Examples

### Read entire response — `.await()`

```cpp
auto stream = xpp::net::TcpStream::connect("example.com:80").await();
stream.write("GET / HTTP/1.0\r\n\r\n", 18).await();
auto data = xpp::io::read_all(stream).await();
printf("%.*s\n", (int)data.size(), data.data());
```

### Read entire response — `co_await` (C++20)

```cpp
xpp::Promise<void> fetch() {
    auto stream = co_await xpp::net::TcpStream::connect("example.com:80");
    co_await stream.write("GET / HTTP/1.0\r\n\r\n", 18);
    auto data = co_await xpp::io::read_all(stream);
    printf("%.*s\n", (int)data.size(), data.data());
}
```

### Copy from TcpStream to File — `.await()`

```cpp
auto stream = xpp::net::TcpStream::connect("example.com:80").await();
stream.write("GET / HTTP/1.0\r\n\r\n", 18).await();

auto file = xpp::fs::File::create("response.txt").await();
xpp::io::copy(stream, file).await();
```

### Copy from TcpStream to File — `co_await` (C++20)

```cpp
xpp::Promise<void> download() {
    auto stream = co_await xpp::net::TcpStream::connect("example.com:80");
    co_await stream.write("GET / HTTP/1.0\r\n\r\n", 18);
    auto file = co_await xpp::fs::File::create("response.txt");
    co_await xpp::io::copy(stream, file);
}
```

### Read from File cursor — `.await()`

```cpp
auto file = xpp::fs::File::open("data.txt").await();
char buf[16];
ssize_t n = file.read(buf, sizeof(buf)).await();  // cursor advance
auto rest = xpp::io::read_all(file).await();       // cursor to EOF
```

### Read from File cursor — `co_await` (C++20)

```cpp
xpp::Promise<void> process() {
    auto file = co_await xpp::fs::File::open("data.txt");
    char buf[16];
    ssize_t n = co_await file.read(buf, sizeof(buf));
    auto rest = co_await xpp::io::read_all(file);
}
```
