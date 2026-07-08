# fs.h — Async Filesystem I/O

## Introduction

`fs.h` provides a minimal async filesystem API built on top of `task.h`'s thread pool. All I/O operations (open, close, read, write, stat, directory management, rename, unlink) are offloaded to worker threads, with completion callbacks delivered on the event loop thread. A synchronous mode is available by passing `cb = NULL` — the call blocks until the operation completes.

## Design Philosophy

1. **Single Request Struct** — All operations share one `xFsReq` struct. The `op` field selects the operation; required fields depend on the op. This avoids function explosion (`xFsOpen`, `xFsRead`, …) and makes batch submission trivial.

2. **Thread Pool Offload** — Filesystem operations are blocking by nature (`pread`, `pwrite`, `stat`, `rename`). Rather than invent async filesystem syscalls, `fs.h` delegates to `task.h`'s N:M thread pool. Worker threads perform the actual syscall; the done callback fires on the event loop thread via `xEventLoopPost`.

3. **Dual Mode (Async / Sync)** — When `req->cb` is non-NULL, `xFsReqSubmit` returns `xErrno_Pending` and the callback is invoked on the event loop thread when the operation completes. When `cb` is NULL, the call blocks the current thread and returns the result directly — useful for scripts, tests, or startup sequences where event loop orchestration is unnecessary.

4. **Zero-Copy Buffer Model** — The caller owns `req->buf` and `req->path`. They must remain valid until the callback fires (async mode) or the call returns (sync mode). No internal copies are made.

5. **Cancellation-Aware** — `xFsReqCancel` delegates to `xWorkCancel`, which uses CAS to atomically cancel a queued-but-not-yet-running task. After cancel, the callback is NOT invoked and both `req` and its buffers can be safely released.

## Architecture

```
Application
    │
    ├── xFsReqSubmit(req) [async, cb != NULL]
    │       │
    │       └── xWorkSubmit(worker_pool, fs_worker, fs_done, req)
    │               │
    │               ├── Enqueued to thread pool
    │               ├── Worker thread: open/read/write/stat/...
    │               └── Done: xEventLoopPost → fs_done → req->cb(req)
    │
    └── xFsReqSubmit(req) [sync, cb == NULL]
            └── fs_worker(req) on calling thread
                   → blocks until complete
```

## API Reference

### Types

| Type | Description |
| --- | --- |
| `xFile` | Opaque file handle. On Unix: `int` cast to `xFile`. On Windows: `HANDLE`. |
| `xFsOp` | Operation enum: `xFsOpOpen`, `xFsOpClose`, `xFsOpRead`, `xFsOpWrite`, `xFsOpStat`, `xFsOpMkdir`, `xFsOpRmdir`, `xFsOpUnlink`, `xFsOpRename` |
| `xFsStat` | Stat result: `size` (off_t), `mode` (int), `mtime` (uint64_t ms), `ctime` (uint64_t ms) |
| `xFsReq` | Per-operation request struct (see below) |
| `xFsFunc` | `typedef void (*xFsFunc)(xFsReq *req)` — completion callback |

### xFsReq

| Field | Type | Description |
| --- | --- | --- |
| `op` | `xFsOp` | Operation to perform |
| `path` | `const char *` | File/directory path (Open, Stat, Mkdir, Unlink, Rename, Rmdir) |
| `buf` | `void *` | Data buffer (Read, Write). For Rename: holds the new path string |
| `len` | `size_t` | Buffer length (Read, Write) |
| `offset` | `off_t` | File offset (Read, Write) |
| `flags` | `int` | Open flags: `O_RDONLY`, `O_CREAT | O_RDWR`, etc. (Open) |
| `mode` | `int` | File/directory mode: `0644`, `0755`, etc. (Open, Mkdir) |
| `file` | `xFile` | File handle (Close, Read, Write) |
| `cb` | `xFsFunc` | Completion callback. NULL = synchronous blocking call |
| `arg` | `void *` | User data passed through to callback |
| `result` | `xErrno` | **Output.** Operation result: `xErrno_Ok` on success |
| `retval` | `ssize_t` | **Output.** Bytes read/written, or -1 on error |
| `done` | `bool` | **Output.** True on the last callback invocation (streaming reads) |
| `stat` | `xFsStat` | **Output.** Stat result (xFsOpStat) |
| `out_file` | `xFile` | **Output.** Opened file handle (xFsOpOpen) |

### Required Fields Per Operation

| Operation | Required Input Fields | Output Fields |
| --- | --- | --- |
| `xFsOpOpen` | `path`, `flags`, `mode`, `cb` | `result`, `retval`, `out_file` |
| `xFsOpClose` | `file`, `cb` | `result`, `retval` |
| `xFsOpRead` | `file`, `buf`, `len`, `offset`, `cb` | `result`, `retval`, `done` |
| `xFsOpWrite` | `file`, `buf`, `len`, `offset`, `cb` | `result`, `retval`, `done` |
| `xFsOpStat` | `path`, `cb` | `result`, `stat` |
| `xFsOpMkdir` | `path`, `mode`, `cb` | `result`, `retval` |
| `xFsOpUnlink` | `path`, `cb` | `result`, `retval` |
| `xFsOpRmdir` | `path`, `cb` | `result`, `retval` |
| `xFsOpRename` | `path` (old), `buf` (new name), `cb` | `result`, `retval` |

### Functions

| Function | Signature | Description |
| --- | --- | --- |
| `xFsReqSubmit` | `xErrno xFsReqSubmit(xFsReq *req)` | Submit an async or sync filesystem operation. Returns `xErrno_Pending` for async (callback will fire), or the result directly for sync. |
| `xFsReqCancel` | `xErrno xFsReqCancel(xFsReq *req)` | Cancel a pending request. Callback will NOT be invoked. Safe with NULL. |

## Usage Examples

### Async Open / Close

```c
#include <x/base/event.h>
#include <x/fs/fs.h>

xFsReq r = {0};
r.op    = xFsOpOpen;
r.path  = "/tmp/example.txt";
r.flags = O_CREAT | O_RDWR | O_TRUNC;
r.mode  = 0644;
r.cb    = [](xFsReq *r) {
    if (r->result == xErrno_Ok) {
        // r->out_file is the open file handle
        xFsReq close = {0};
        close.op   = xFsOpClose;
        close.file = r->out_file;
        close.cb   = [](xFsReq *r) { xEventLoopStop(xEventLoopCurrent()); };
        xFsReqSubmit(&close);
    }
};
xFsReqSubmit(&r);  // returns xErrno_Pending
xEventLoopRun(loop, X_RUN_DEFAULT);
```

### Sync Write / Read

```c
xFsReq r = {0};

// Open
r.op    = xFsOpOpen;
r.path  = "/tmp/data.bin";
r.flags = O_CREAT | O_RDWR | O_TRUNC;
r.mode  = 0644;
assert(xFsReqSubmit(&r) == xErrno_Ok);
xFile f = r.out_file;

// Write
const char *msg = "hello fs!";
r.op   = xFsOpWrite;
r.file = f;
r.buf  = (void *)msg;
r.len  = strlen(msg);
assert(xFsReqSubmit(&r) == xErrno_Ok);

// Read back
char buf[64] = {0};
r.op     = xFsOpRead;
r.buf    = buf;
r.len    = sizeof(buf);
r.offset = 0;
assert(xFsReqSubmit(&r) == xErrno_Ok);
assert(strcmp(buf, "hello fs!") == 0);

// Close
r.op   = xFsOpClose;
xFsReqSubmit(&r);
```

### Stat

```c
xFsReq r = {0};
r.op   = xFsOpStat;
r.path = "/tmp";
assert(xFsReqSubmit(&r) == xErrno_Ok);

printf("size: %lld, mode: %o, mtime: %llu\n",
       r.stat.size, r.stat.mode, r.stat.mtime);
```

### Directory Operations

```c
xFsReq r = {0};

// Create directory
r.op   = xFsOpMkdir;
r.path = "/tmp/my_dir";
r.mode = 0755;
assert(xFsReqSubmit(&r) == xErrno_Ok);

// Rename
r.buf = (void *)"/tmp/my_dir_renamed";  // new path in buf
r.op  = xFsOpRename;
assert(xFsReqSubmit(&r) == xErrno_Ok);

// Remove directory
r     = (xFsReq){0};
r.op  = xFsOpRmdir;
r.path = "/tmp/my_dir_renamed";
assert(xFsReqSubmit(&r) == xErrno_Ok);
```

## Platform Notes

| Aspect | Unix | Windows |
| --- | --- | --- |
| `xFile` | `int` fd cast to `xFile` | `HANDLE` from `CreateFile` |
| Open flags | POSIX `O_CREAT | O_RDWR` etc. | Windows `_O_CREAT | _O_RDWR` etc. |
| Stat | POSIX `stat(2)` | Windows `_stat64` |
| Thread pool | pthread-based `task.h` worker | Same via `task.h` abstraction |

## Thread Safety

- `xFsReqSubmit`: **Thread-safe** — can be called from any thread. The worker executes on a pool thread; the callback fires on the event loop thread.
- `xFsReqCancel`: **Thread-safe** — delegates to `xWorkCancel` which uses CAS.
- **Buffers and paths**: The caller is responsible for ensuring `req->path`, `req->buf`, and the `xFsReq` itself remain valid until the callback fires (async) or the call returns (sync). No internal copies are made.

## Error Handling

| Return / Result | Meaning |
| --- | --- |
| `xErrno_Pending` | Async submission accepted. Callback will fire. |
| `xErrno_Ok` | Operation completed successfully (sync mode) or callback reports success. |
| `xErrno_InvalidArg` | `req` is NULL, or required fields are missing. |
| `xErrno_SysError` | Underlying syscall failed (`open`, `read`, `write`, `stat`, etc.). Check `errno`. |
| `xErrno_NoMemory` | Thread pool queue is full or allocation failed. |

## See Also

- `task.h` — Thread pool (`xWork`, `xWorkSubmit`, `xWorkCancel`) that powers the async path
- `event.h` — Event loop where callbacks are delivered
- `error.h` — Error code definitions (`xErrno`)
