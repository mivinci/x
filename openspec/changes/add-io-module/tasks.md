## 1. AsyncFd core (io/async_fd.h)

- [x] 1.1 Define `AsyncFd` class: `int m_fd`, `xEventSource m_src`, `bool m_readable`, `bool m_writable`, `PromiseResolver<void> m_read_waiter`, `PromiseResolver<void> m_write_waiter`
- [x] 1.2 Constructor: `xEventAdd(fd, Read|Write, on_event, this)` — register once, edge-triggered
- [x] 1.3 `on_event` static callback: set readiness bools, resolve pending waiter if present, clear readiness after resolve
- [x] 1.4 `readable()`: if `m_readable` → clear + resolve immediately; else store `PromiseResolver` in `m_read_waiter` via AsyncReadAdapter
- [x] 1.5 `writable()`: symmetric to `readable()`
- [x] 1.6 `close()`: `xEventDel`, wake both waiters, set `m_fd = -1`
- [x] 1.7 Destructor: `xEventDel` if registered, wake waiters (does NOT `::close(fd)`)
- [x] 1.8 Move constructor/assignment: transfer ownership, source becomes tombstone
- [x] 1.9 `fd()`, `is_closed()` accessors

## 2. Adapters (io/async_fd_adapter.h)

- [x] 2.1 `AsyncReadAdapter`: constructor receives `PromiseResolver<void>` + `AsyncFd*`, stores resolver in `m_read_waiter`
- [x] 2.2 `AsyncWriteAdapter`: constructor receives `PromiseResolver<void>` + `AsyncFd*`, stores resolver in `m_write_waiter`
- [x] 2.3 `readable()` implementation: `return xpp::adapt<void, AsyncReadAdapter>(this)`
- [x] 2.4 `writable()` implementation: `return xpp::adapt<void, AsyncWriteAdapter>(this)`

## 3. Free functions (io/async_fd.h)

- [x] 3.1 `read(AsyncFd&, void* buf, size_t len)`: try `::recv`, fast path `resolve(n)`, slow path `readable().then(recv)`
- [x] 3.2 `write(AsyncFd&, const void* buf, size_t len)`: try `::send`, same pattern
- [x] 3.3 `read_full(AsyncFd&, void* buf, size_t len)`: loop `read()` until full or EOF (shared_ptr<std::function> for recursion)
- [x] 3.4 `write_all(AsyncFd&, const void* buf, size_t len)`: loop `write()` until all sent

## 4. Tests (io/async_fd_test.cpp)

- [x] 4.1 Create AsyncFd from a socketpair, verify registration
- [x] 4.2 Write to one end, `readable()` on other end resolves immediately
- [x] 4.3 `readable()` on empty socket — pending, then write from another thread, resolves
- [x] 4.4 `read()` fast path: data available, resolves immediately
- [x] 4.5 `read()` slow path: EAGAIN → `readable()` → resolves with data
- [x] 4.6 `write()` fast path: buffer space available, resolves immediately
- [x] 4.7 `write()` slow path: fill buffer → EAGAIN → `writable()` → resolves
- [x] 4.8 `read_full()`: read exact N bytes across multiple chunks
- [x] 4.9 `write_all()`: write N bytes across multiple chunks
- [x] 4.10 `close()`: pending `readable()` resolves (EOF)
- [x] 4.11 Promise destroyed before event: no crash (ArcWeak safety)
- [x] 4.12 Move semantics: moved-from is tombstone

## 5. Docs

- [ ] 5.1 Create `docs/libxpp/io.md` — AsyncFd API, fast/slow path, comparison with tokio's PollEvented/IoSource
- [ ] 5.2 Update `docs/SUMMARY.md` — add io page
- [ ] 5.3 Update `docs/libxpp/README.md` — add io to module list
