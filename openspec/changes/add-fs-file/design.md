## Context

libx's `xfs` module provides async file I/O via thread pool offload. The API is callback-based:

```c
xFsReq req = {.op = xFsOpRead, .file = f, .buf = buf, .len = n, .offset = 0, .cb = callback};
xFsReqSubmit(&req);
// callback runs on event loop thread when done
```

xpp needs a Promise-based wrapper so file I/O composes with `.then()`, `co_await`, `all()`/`race()`. The Adapter pattern (`AdapterPromiseNode` + `PromiseResolver`) is the natural bridge — same pattern used by `TimerAdapter` and `WorkAdapter`.

## Goals / Non-Goals

**Goals:**
- `xpp::fs::File` with Promise-returning methods: `open`, `read`, `write`, `close`, `sync_all`, `stat`
- Convenience: `read_all()`, `read_to_string()`, `write_all()`
- Free functions: `stat(path)`, `read(path)`, `write(path, buf, len)`
- RAII: destructor closes file synchronously
- Buffer overloads: `void* + size_t` (base), `Span<uint8_t>` (type-safe)
- Path: `const char*` (no Path class)

**Non-Goals:**
- `Path` class (use `std::filesystem::path` or `const char*`)
- `seek`/`tell` (use pread/pwrite with explicit offset)
- Directory operations (readdir, opendir) — separate change
- `AsyncRead`/`AsyncWrite` trait equivalents — File methods are sufficient
- `OpenOptions` builder — use `open_with(path, flags, mode)` for custom flags
- File locking (flock/fcntl) — separate change

## Decisions

### D1: FsAdapter bridges xFsReq to PromiseResolver

```cpp
template <class T>
class FsAdapter {
    xFsReq           m_req;
    PromiseResolver<T> m_resolver;
    bool              m_done = false;
    
    // Constructor: fill m_req, set cb, submit
    // Callback: resolve PromiseResolver, set m_done
    // Destructor: if !m_done, xFsReqCancel
};
```

Same lifecycle as `TimerAdapter` and `WorkAdapter`. The callback runs on the event loop thread (guaranteed by libx), so `PromiseResolver::resolve()` is safe.

### D2: pread/pwrite — no seek

All `read`/`write` take explicit `off_t offset`. This matches libx's `xFsReq.offset` field. Benefits:
- Thread-safe (no shared file position)
- No need for `seek` operation
- Simpler API (one less concept)

### D3: RAII with sync close

```cpp
~File() {
    if (m_handle) {
        xFsReq req = {.op = xFsOpClose, .file = m_handle, .cb = NULL};
        xFsReqSubmit(&req);  // cb=NULL → synchronous blocking
    }
}
```

Blocking in destructor is acceptable — file close is fast (just `close(fd)`). If user wants async close, they call `close()` explicitly before dropping.

### D4: Buffer overloads

Base: `void* buf, size_t len` — matches POSIX, matches xFsReq, works with all buffer types.
Span: `Span<uint8_t> buf` — for users who want type safety. Delegates to base.

```cpp
Promise<ssize_t> read(void* buf, size_t len, off_t offset);
Promise<ssize_t> read(Span<uint8_t> buf, off_t offset) {
    return read(buf.data(), buf.size(), offset);
}
```

### D5: Error handling — ssize_t with negative = error

```cpp
Promise<ssize_t> read(...);  // >= 0 = bytes read, < 0 = -errno
```

Matches POSIX convention. User checks `result >= 0`. Avoids `Result<ssize_t, xErrno>` overhead for the common success path.

### D6: Convenience methods allocate their own buffer

```cpp
Promise<std::vector<uint8_t>> read_all();      // stat → allocate → read
Promise<std::string> read_to_string();          // read_all → convert to string
Promise<void> write_all(const void* buf, size_t len);  // loop write until all sent
```

`read_all` calls `stat` to get file size, allocates a vector, reads in one or more chunks.

## Risks / Trade-offs

- **[Sync close in destructor]** Blocks the calling thread. Acceptable — `close(fd)` is near-instant. User can call `close()` explicitly for async.
- **[No seek]** Users who need sequential I/O must track offset manually. Acceptable — pread/pwrite is the modern pattern, and libx doesn't support seek either.
- **[Negative = error]** Less type-safe than `Result<T, E>`. Acceptable — matches POSIX, and xpp's `Result` is for higher-level error handling, not system calls.
- **[FsAdapter lifetime]** The adapter must outlive the xFsReq. It's owned by `AdapterPromiseNode`, which is owned by the Promise chain. If the Promise is dropped, `~AdapterPromiseNode` cancels the request. Same pattern as TimerAdapter.
