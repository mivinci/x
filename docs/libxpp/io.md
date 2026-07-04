# I/O

## Introduction

`xpp::io::AsyncFd` provides reactive async I/O for non-blocking file descriptors. It registers an fd with the event loop once (edge-triggered, Read|Write), tracks readiness internally, and provides `readable()`/`writable()` as `Promise<void>`.

Free functions `read()`/`write()` combine a fast-path syscall (zero Promise overhead when data is available) with a readiness wait on EAGAIN. `read_full()`/`write_all()` loop until complete.

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

int sv[2];
socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
xpp::io::AsyncFd io(sv[0]);

// Fast path: data already available
write(sv[1], "hello", 5);
char buf[64] = {};
ssize_t n = xpp::io::read(io, buf, sizeof(buf)).wait();
// n == 5
```

## Design Philosophy

1. **Register once, not per operation** — `AsyncFd` registers with `xEventAdd` once in the constructor. Operations check readiness bools first (fast path), only storing a `PromiseResolver` when EAGAIN occurs. No `xEventAdd`/`xEventDel` churn.

2. **Fast path: zero Promise overhead** — `read()`/`write()` try the syscall immediately. If data is available (the common case), the result is returned via `resolve(n)` with no Promise chain, no event registration, no waiting.

3. **Adapter pattern, not custom PromiseNode** — `readable()`/`writable()` use `adapt<void, AsyncReadAdapter>()`. The adapter stores a `PromiseResolver<void>` in `AsyncFd`'s waiter slot. When `on_event` fires, it calls `resolver.resolve()`, which triggers the waker in `ResolveState`. Same pattern as `TimerAdapter`, `WorkAdapter`, `FsOpenAdapter`.

4. **Single-threaded** — All operations run on the event loop thread. Plain `bool` for readiness, no atomics or mutex. When multi-threaded scheduler is added, upgrade to `atomic<uint8_t>` + `mutex<Waiters>` + double-check-under-lock.

5. **Does not own fd** — `AsyncFd` registers/deregisters with the event loop but does NOT `::close(fd)`. The caller owns the fd. This allows wrapping fds from any source (TCP, pipe, eventfd, etc.).

## Architecture

```
read(io, buf, len)
    │
    ├── recv(fd, buf, len) → n >= 0
    │       └── resolve(n)              ← fast path, zero overhead
    │
    └── recv returns EAGAIN
            └── io.readable()
                    ├── m_readable == true?
                    │       └── resolve()  ← already ready, no wait
                    └── store PromiseResolver in m_read_waiter
                            └── on_event fires (fd readable)
                                    └── m_read_waiter.resolve()
                                    └── .then(recv) retries
```

```
AsyncFd (per-fd, registered once)
├── xEventSource (persistent, edge-triggered, Read|Write)
├── bool m_readable / m_writable
├── PromiseResolver<void> m_read_waiter / m_write_waiter
└── on_event callback:
      if Read: set m_readable or resolve m_read_waiter
      if Write: set m_writable or resolve m_write_waiter
```

## API Reference

### AsyncFd

| Method | Returns | Description |
|--------|---------|-------------|
| `AsyncFd(fd)` | | Register fd with event loop (edge-triggered, Read\|Write) |
| `readable()` | `Promise<void>` | Resolve when fd is readable. Immediate if already ready |
| `writable()` | `Promise<void>` | Resolve when fd is writable. Immediate if already ready |
| `close()` | `void` | Deregister, wake pending waiters. Does NOT close fd |
| `fd()` | `int` | Raw file descriptor |
| `is_closed()` | `bool` | True after close() or move |

### Free functions

| Function | Returns | Description |
|----------|---------|-------------|
| `read(io, buf, len)` | `Promise<ssize_t>` | Async recv. Fast path: try immediately. EAGAIN: wait readable |
| `write(io, buf, len)` | `Promise<ssize_t>` | Async send. Same fast/slow pattern |
| `read_full(io, buf, len)` | `Promise<ssize_t>` | Read exactly len bytes (loops). Returns bytes read |
| `write_all(io, buf, len)` | `Promise<void>` | Write all bytes (loops until complete) |

## Usage Examples

### Basic read

```cpp
xpp::io::AsyncFd io(fd);
char buf[1024];
ssize_t n = xpp::io::read(io, buf, sizeof(buf)).wait();
```

### Read with then() chain

```cpp
xpp::io::read(io, buf, 1024).then([](ssize_t n) {
    // process n bytes
    return n;
}).wait();
```

### Wait for readability without reading

```cpp
io.readable().then([&]() {
    // fd is readable, do something custom
}).wait();
```

### Write all

```cpp
xpp::io::write_all(io, data, sizeof(data)).wait();
```

### Close wakes pending waiters

```cpp
xpp::io::AsyncFd io(fd);
// ... later ...
io.close(); // any pending readable()/writable() resolves immediately
// caller still needs to ::close(fd)
```

## Comparison

| Feature | `xpp::io::AsyncFd` | tokio `PollEvented` / `IoSource` | moo `PollEvented` |
|---------|---------------------|-----------------------------------|---------------------|
| Registration | once (persistent) | once (persistent) | once (persistent) |
| Readiness tracking | `bool` (single-thread) | `atomic` + mutex | `atomic` + mutex |
| Wait mechanism | `PromiseResolver` (Adapter) | `Waker` (custom PromiseNode) | `Waker` (custom PromiseNode) |
| Thread safety | single-thread | multi-thread | multi-thread |
| Fast path | try syscall, zero overhead | try syscall, zero overhead | try syscall, zero overhead |
| fd ownership | caller owns | caller owns | caller owns |

## Implementation Notes

### Edge-triggered readiness

`xEventAdd` uses edge-triggered by default. After `recv` returns EAGAIN, the fd is not readable. When data arrives, the edge fires and `on_event` is called. The readiness bool is set, and the next `readable()` call resolves immediately.

If `on_event` fires while a `PromiseResolver` is stored in `m_read_waiter`, the resolver is called directly (readiness is consumed, not stored as a bool). This avoids a wasted round-trip.

### PromiseResolver safety

`PromiseResolver<void>` holds `ArcWeak<ResolveState>`. If the Promise is destroyed before the event fires:
1. `AdapterPromiseNode` destroyed → `Arc<ResolveState>` dropped
2. `on_event` fires → `m_read_waiter.resolve()` → `ArcWeak::upgrade()` fails → no-op
3. No use-after-free

### read_full / write_all recursion

`read_full` and `write_all` use `shared_ptr<std::function>` for recursive Promise chaining. The `shared_ptr` keeps the step function alive until the Promise chain completes. This avoids the dangling reference problem of capturing a local `std::function` by reference.

### Move semantics

Move constructor/assignment re-registers with `xEventAdd` because the event callback's `arg` pointer must point to the new `AsyncFd` object. `xEventDel` on the old source, `xEventAdd` on the new. The old object becomes a tombstone (fd == -1).
