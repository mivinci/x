## Why

libx already has `xfs` — an async filesystem module using thread pool offload with callback-style API (`xFsReqSubmit` + `xFsFunc` callback). xpp users need a Promise-based wrapper so file I/O integrates naturally with `.then()` chains, `co_await`, and `all()`/`race()` combinators.

## What Changes

- Add `xpp::fs::File` — RAII async file handle wrapping `xFile` from libx
- Add `xpp::fs::Stat` — file metadata struct (maps `xFsStat`)
- Add `xpp::fs::FsAdapter<T>` — internal Adapter that bridges `xFsReq` callbacks to `PromiseResolver<T>`
- File operations return `Promise<T>`: `open`, `create`, `read`, `write`, `close`, `sync_all`, `stat`
- Convenience methods: `read_all()`, `read_to_string()`, `write_all()`
- Free functions: `xpp::fs::stat(path)`, `xpp::fs::read(path)`, `xpp::fs::write(path, buf, len)`
- Path parameters: `const char*` (no Path class — use `std::filesystem::path` if needed)
- Buffer parameters: C-style `void* + size_t` as base, `Span<uint8_t>` overload for type safety
- RAII: destructor calls synchronous close (blocking but safe)
- No `seek` — use `pread`/`pwrite` with explicit offset (thread-safe, no shared file position)

## Capabilities

### New Capabilities
- `fs-file`: Async file I/O via `xpp::fs::File` — Promise-based wrapper over libx `xfs` module. Open, read, write, close, stat, sync, plus convenience helpers.

### Modified Capabilities
(none)

## Impact

- **New files**: `libxpp/xpp/fs/file.h`, `libxpp/xpp/fs/file.cpp` (FsAdapter implementation), `libxpp/xpp/fs/file_test.cpp`
- **Dependencies**: `libx/x/fs` (xfs module), `xpp/promise.h`, `xpp/span.h`
- **No breaking changes**: entirely new module
- **Docs**: new `docs/libxpp/fs.md`, update `docs/SUMMARY.md` and `docs/libxpp/README.md`
