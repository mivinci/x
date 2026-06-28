## Context

libx provides async networking (xhttp, xdns, xp2p) but no async filesystem I/O. The event loop's `xWorkSubmit` already provides thread pool offload with cancellation support (`on_cancel`), making async FS a natural fit.

## Goals / Non-Goals

**Goals:**
- Open, close, read, write, stat, mkdir, unlink, rename — all async
- Streaming reads via multi-call callback with `done` flag
- Synchronous mode (`cb = NULL`)
- Cancel via `xFsReqCancel` → `xWorkCancel`

**Non-Goals:**
- Watching files for changes (EVFILT_VNODE — future)
- chmod/chown/utime/symlink (v2)
- sendfile / mmap (v2)

## Decisions

### 1. Unified xFsReqSubmit pattern

Single API surface borrowed from libuv + our earlier design:

```c
xErrno xFsReqSubmit(xFsReq *req);
xErrno xFsReqCancel(xFsReq *req);
```

All operations distinguished by `req->op`. Callback fires on event loop thread.

### 2. Thread pool implementation

Each `xFsReqSubmit` maps to one `xWorkSubmit` call. The worker thread calls the corresponding POSIX API, and the done callback (`xFsFunc`) fires on the event loop thread. `on_cancel = NULL` since buffers are caller-owned.

### 3. Streaming reads

The `done` flag in the callback enables multi-call delivery:

```c
static void on_read(xFsReq *r) {
    xHttpCtxWrite(ctx, r->buf, r->retval);
    if (r->done) {
        xFsReqSubmit(&(xFsReq){ .op = xFsOpClose, .file = r->file });
    }
}
```

### 4. Layering

```
xfs depends on: xbase (xWorkSubmit, xTaskGroup)
xfs depended by: nothing initially
```

## Risks / Trade-offs

- Small files may not need thread pool — future optimization: direct I/O below threshold
- Thread pool contention with other xWorkSubmit users — configurable group via v2 API
