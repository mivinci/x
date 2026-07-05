# I/O

## Introduction

`xpp::io` provides reactive async I/O for non-blocking file descriptors, plus a structured error type shared across all I/O modules (net, fs, etc.).

```cpp
#include <xpp/io/async_fd.h>
#include <xpp/io/error.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

int sv[2];
socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
xpp::io::AsyncFd io(sv[0]);

write(sv[1], "hello", 5);
char buf[64] = {};
ssize_t n = xpp::io::read(io, buf, sizeof(buf)).wait();
```

## Modules

- [AsyncFd](async_fd.md) — Reactive I/O wrapper: register fd once, `readable()`/`writable()` as `Promise<void>`, fast-path `read()`/`write()`.
- [I/O Error](error.md) — `io::Error`: niche-optimized (4-byte) error type with `ErrorKind`, `raw_os_error()`, `raw_xerrno()`. Mirrors Rust's `std::io::Error`.

- [Utilities](util.md) — `read_all` and `copy`: duck-typed template functions. C++20 coroutine loops with 8KB stack buffers.
- [BufReader](buf_reader.md) — `BufReader<R>`: buffered async reader. Reduces per-call Promise overhead for small reads.
- [BufWriter](buf_writer.md)
- [Take](take.md)
- [Empty](empty.md)
- [Sink](sink.md)
- [Duplex](duplex.md) — `BufWriter<W>`: buffered async writer. Coalesces small writes, explicit `flush()`.
