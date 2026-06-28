## Why

libx has async networking (xhttp, xdns, xp2p) but no async filesystem I/O. Static file serving, config loading, and log rotation all currently require blocking POSIX calls on the event loop thread. An async FS module fills this gap using xbase's thread pool for POSIX I/O offload.

## What Changes

- New module `libx/x/fs/` — async filesystem operations
- Unified `xFsReq` request pattern with `xFsReqSubmit` / `xFsReqCancel`
- 8 operations: open, close, read, write, stat, mkdir, unlink, rename
- Streaming reads via multi-call handler with `done` flag (inspired by GCD dispatch_io)
- Synchronous mode when `cb = NULL`
- Depends on xbase only (xWorkSubmit + xTaskGroup)

## Capabilities

### New Capabilities

- `async-fs`: Async filesystem I/O via `xFsReqSubmit` / `xFsReqCancel` with thread pool offload.

## Impact

- `libx/x/fs/fs.h` — public API
- `libx/x/fs/fs.c` — implementation
- `libx/x/fs/CMakeLists.txt` — build
- No changes to existing modules
