## Why

xpp needs a reactive I/O layer to bridge libx's event loop (`xEventAdd`/`xEventDel`) with the Promise system. This is the foundation for async TCP recv/send, pipes, Unix sockets, and any fd-based I/O. Without it, `xpp::net::TcpConn::recv()` has no way to return `Promise<ssize_t>` — there's no mechanism to "wait for fd readiness, then retry syscall" in the Promise world.

## What Changes

- Add `xpp::io::AsyncFd` — RAII wrapper that registers an fd with the event loop once (edge-triggered, Read|Write), tracks readiness via plain bools (single-threaded), and provides `readable()`/`writable()` returning `Promise<void>` via the Adapter pattern.
- Add `AsyncReadAdapter` / `AsyncWriteAdapter` — store `PromiseResolver<void>` in `AsyncFd`'s waiter slot; resolved by the event callback when the fd becomes ready.
- Add free functions `xpp::io::read(fd, buf, len)` / `xpp::io::write(fd, buf, len)` — try syscall immediately (fast path, zero Promise overhead); on EAGAIN, chain `readable().then(retry)`.
- Add `xpp::io::read_full(fd, buf, len)` / `xpp::io::write_all(fd, buf, len)` — loop until complete.
- Single-threaded only (no atomics/mutex). When multi-threaded scheduler is added later, upgrade `AsyncFd` to use `atomic<uint8_t>` + `mutex<Waiters>` + double-check-under-lock (same as moo's `ScheduledIo`).

## Capabilities

### New Capabilities
- `io`: Reactive async I/O for non-blocking file descriptors. `AsyncFd` registers with event loop, tracks readiness, provides `readable()`/`writable()` Promises. Free functions `read`/`write`/`read_full`/`write_all` combine fast-path syscall with readiness wait.

### Modified Capabilities
(none)

## Impact

- **New files**: `libxpp/xpp/io/async_fd.h`, `libxpp/xpp/io/async_fd_adapter.h`, `libxpp/xpp/io/async_fd_test.cpp`
- **Dependencies**: `libx/x/base/event.h` (xEventAdd/xEventDel/xEventLoopCurrent), `xpp/promise.h`, `xpp/arc.h`
- **No breaking changes**: entirely new module
- **Docs**: new `docs/libxpp/io.md`, update `docs/SUMMARY.md` and `docs/libxpp/README.md`
