## Context

The current xfs module (`fs.c`) uses POSIX APIs (`pread`, `pwrite`) for single-shot async I/O. The POSIX backend compiles on macOS and Linux. On Windows, these APIs are unavailable — the equivalent is `ReadFile`/`WriteFile` with `OVERLAPPED` structs, or `_read`/`_write` with manual offset management.

The project uses a backend selection pattern already established for event loops (kqueue/epoll/poll/WSAPoll) and TLS (openssl/mbedtls). xfs will follow the same pattern: `fs.c` remains the POSIX implementation, and a new `fs_win32.c` provides the Windows implementation.

## Goals / Non-Goals

**Goals:**
- Add a Windows backend for xfs that supports the same 8 operations: open, close, read, write, stat, mkdir, unlink, rename
- Async mode (thread-pool offload) must work on Windows, same as POSIX
- Sync mode (`cb = NULL`) must work for all operations
- Existing POSIX behavior unchanged — Windows is additive

**Non-Goals:**
- IOCP (I/O Completion Ports) or true async overlapped I/O — thread-pool offload (`xWorkSubmit`) is sufficient for v1
- Windows-specific file permission semantics — use minimal mapping (e.g., `_S_IREAD | _S_IWRITE` for 0644)
- Long path support (`\\?\` prefix) — requires careful forward-slash normalization but is a follow-up

## Decisions

### Decision 1: Thread-pool offload, not IOCP

Use `xWorkSubmit` to offload `ReadFile`/`WriteFile` to a background thread, same as the POSIX backend. This avoids the complexity of IOCP event handling and keeps the architecture symmetric.

**Rationale**: The xfs module's `xErrno_Pending` return already expects the result to be delivered via `xEventLoopPost`. IOCP would require a fundamentally different callback model. Thread-pool is simpler and sufficient for the current use cases (block-sized cache reads/writes).

### Decision 2: Win32 API for I/O, CRT for metadata

Use `CreateFileA` / `ReadFile` / `WriteFile` / `CloseHandle` for open/close/read/write operations, matching libuv's approach. `ReadFile`/`WriteFile` accept an `OVERLAPPED.Offset` that carries the file position, making positional I/O naturally thread-safe — no global lock needed.

For metadata operations (stat, mkdir, unlink, rename), use CRT (`_stat64`, `_mkdir`) or simple Win32 wrappers (`DeleteFileA`, `MoveFileA`) — these are infrequent and need no offset tracking.

**Rationale**: `ReadFile` + `OVERLAPPED.Offset` eliminates the thread-safety problem of `_lseeki64` + `_read`/`_write`. This is the same approach libuv and Node.js use. The `xFile` opaque handle on POSIX holds `(void*)(intptr_t)fd`. On Windows, callers (cache.c) open files with CRT `open()`, and the Win32 backend converts the CRT fd to a Win32 HANDLE via `_get_osfhandle()` — transparent to callers, no API change.

**Alternative considered**: CRT `_open` / `_read` / `_write` with `CRITICAL_SECTION` serialization. Rejected — the lock adds complexity and CRT `_lseeki64` + `_read` is not truly atomic even with a mutex if the fd is shared across processes.

### Decision 3: `stat` via `_stat64`

Use `_stat64` for the `xFsOpStat` operation, mapping fields as follows:
- `st.st_size` → `r->stat.size`
- `st.st_mode` → best-effort mapping (`_S_IFREG`, `_S_IFDIR`)
- `st.st_mtime` → `r->stat.mtime` (in milliseconds)
- `st.st_ctime` → `r->stat.ctime`

### Decision 4: Backend selection via CMake `#ifdef X_HAS_FS_WIN32`

Add `cmake/DetectWin32.cmake` that sets `X_HAS_FS_WIN32` on Windows targets. The `fs_win32.c` is guarded with `#ifdef X_HAS_FS_WIN32`. The existing `fs.c` is guarded with `#ifndef X_HAS_FS_WIN32` (or keep using `!defined(_WIN32)` and let `fs_win32.c` be selected as the sole source on Windows).

**Rationale**: Simplest approach — on Windows, compile `fs_win32.c` instead of `fs.c`. No runtime dispatch needed.

## Risks / Trade-offs

- **`_get_osfhandle` bridge fragility**: The caller (cache.c) opens files with CRT `open()`, and xfs converts the fd to HANDLE via `_get_osfhandle`. If the CRT fd is closed independently, the HANDLE becomes invalid. Mitigation: the cache module only closes fds via xFsOpClose, never directly. The fd/HANDLE lifecycle is managed exclusively by xfs.
- **HANDLE vs fd confusion in `xFile`**: On POSIX, `xFile` holds `(void*)(intptr_t)fd`. On Windows with CRT callers, the same `(void*)(intptr_t)fd` holds a CRT fd, which xfs converts to HANDLE on each I/O call. The conversion overhead is negligible (a simple CRT table lookup).
- **`DeleteFileA` on open files**: Windows doesn't allow deleting a file while it's open. Mitigation: close the file handle before deletion, same as POSIX.
- **CRT fd limits**: The Microsoft CRT has a default limit of 2048 open file descriptors. Mitigation: dlproxy keeps one fd per clip, typical usage is well under 100 fds.
