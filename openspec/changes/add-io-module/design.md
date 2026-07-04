## Context

libx's event loop provides `xEventAdd(fd, mask, callback, arg)` — registers an fd for edge-triggered readiness notifications. The callback fires on the event loop thread when the fd becomes readable/writable.

xpp's Promise system is poll-based: `PromiseNode::poll(waker) → Option<T>`. The Adapter pattern (`AdapterPromiseNode<T, Adapter>` + `PromiseResolver<T>`) bridges callback-style async ops to Promises.

The missing piece: a per-fd object that registers once with `xEventAdd`, tracks readiness, and provides `readable()`/`writable()` as Promises. This is the foundation for async TCP I/O.

## Goals / Non-Goals

**Goals:**
- `AsyncFd`: RAII wrapper, registers fd once (edge-triggered, Read|Write), tracks readiness via bool, provides `readable()`/`writable()` → `Promise<void>`
- `AsyncReadAdapter` / `AsyncWriteAdapter`: store `PromiseResolver<void>` in `AsyncFd`, resolved by event callback
- Free functions: `read(fd, buf, len)`, `write(fd, buf, len)`, `read_full`, `write_all`
- Fast path: try syscall first, zero Promise overhead if data available
- Slow path: EAGAIN → `readable().then(retry)` — one Promise chain per EAGAIN

**Non-Goals:**
- Thread safety (single-threaded only; upgrade to atomics + mutex when multi-threaded scheduler arrives)
- `AsyncRead`/`AsyncWrite` traits (like Rust's tokio traits)
- `PollEvented` generic wrapper (we only need raw fd, not generic over transport)
- Direct integration with `xpp::fs::File` (fs uses thread-pool offload, not event-driven I/O)

## Decisions

### D1: Single-threaded, plain bools, no mutex

```cpp
class AsyncFd {
    bool m_readable = false;
    bool m_writable = false;
    PromiseResolver<void> m_read_waiter;
    PromiseResolver<void> m_write_waiter;
};
```

No atomics, no mutex. The event callback and all Promise operations run on the same event loop thread. When multi-threaded scheduler is added, upgrade to `atomic<uint8_t>` + `mutex<Waiters>` + double-check-under-lock (moo's ScheduledIo pattern).

### D2: Adapter pattern, not custom PromiseNode

```cpp
Promise<void> readable() const {
    return xpp::adapt<void, AsyncReadAdapter>(m_self);
}
```

`AsyncReadAdapter` stores the `PromiseResolver<void>` in `AsyncFd`'s waiter slot. When `on_event` fires, it calls `resolver.resolve()`, which triggers the waker in `ResolveState`, which re-polls `AdapterPromiseNode`, which sees the resolved state and returns `Some(Void{})`.

No custom `PromiseNode` needed. Same pattern as `TimerAdapter`, `WorkAdapter`, `FsOpenAdapter`.

### D3: Fast path — try syscall before creating Promise

```cpp
Promise<ssize_t> read(AsyncFd& io, void* buf, size_t len) {
    ssize_t n = ::recv(io.fd(), buf, len, 0);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        return xpp::resolve(n);        // fast path: zero Promise chain

    // slow path: EAGAIN
    return io.readable().then(
        [fd = io.fd(), buf, len] { return ::recv(fd, buf, len, 0); });
}
```

Fast path (data available): one `resolve(n)` — no event registration, no Promise chain.

Slow path (EAGAIN): one `adapt()` + one `.then()`. The `readable()` Promise resolves when `on_event` fires, then `.then()` retries the syscall. If the retry also returns EAGAIN (rare with edge-triggered), the result is a negative errno — caller can retry by calling `read()` again.

### D4: PromiseResolver safety after Promise destruction

`PromiseResolver` holds `ArcWeak<ResolveState>`. If the Promise is destroyed before the event fires:
1. `AdapterPromiseNode` destroyed → `Arc<ResolveState>` dropped
2. `on_event` fires → `m_read_waiter.resolve()` → `ArcWeak::upgrade()` fails → no-op
3. `m_read_waiter` cleared to empty

No use-after-free. Same safety as all other adapters.

### D5: AsyncFd owns the fd lifecycle

```cpp
~AsyncFd() {
    if (m_src) xEventDel(m_src);
    // Does NOT close(fd) — caller owns the fd
}
```

`AsyncFd` registers/deregisters with the event loop but does NOT close the fd. The caller (e.g. `TcpConn`) owns the fd and is responsible for closing it. This separation allows `AsyncFd` to wrap fds from any source (TCP, pipe, eventfd, etc.).

`close()` method: deregisters from event loop + wakes both waiters with EOF. Does NOT close the fd — caller still needs to `::close(fd)`.

## Risks / Trade-offs

- **[Single-threaded only]** No atomics/mutex. Must upgrade when adding multi-threaded scheduler. The upgrade path is clear: replace bools with `atomic<uint8_t>`, add `mutex<Waiters>`, add double-check pattern. Same as moo's ScheduledIo.
- **[One retry only]** `read()` retries the syscall once after `readable()`. If EAGAIN again (rare with edge-triggered), returns negative errno. Caller must call `read()` again. This is simpler than a poll loop and sufficient for most cases. `read_full` loops internally.
- **[No concurrent read+write on same fd]** Single `m_read_waiter` / `m_write_waiter` slot each. Multiple concurrent reads on the same fd would overwrite the waiter. Acceptable — TCP connections typically don't have concurrent reads. If needed later, use a waker queue.
- **[fd not closed by AsyncFd]** Caller must close the fd. If the caller forgets, the fd leaks. Documented in the API.
