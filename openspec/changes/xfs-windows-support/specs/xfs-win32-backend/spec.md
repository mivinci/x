## ADDED Requirements

### Requirement: Windows async file I/O via thread pool

The xfs module SHALL support all 8 operations (open, close, read, write, stat, mkdir, unlink, rename) on Windows using the Microsoft CRT APIs (`_open`, `_read`, `_write`, `_close`, `_stat64`, `_mkdir`, `_unlink`, `rename`). Async operations SHALL be offloaded via `xWorkSubmit` to the thread pool, matching the POSIX backend's behavior.

#### Scenario: Synchronous write on Windows

- **WHEN** `xFsReqSubmit` is called with `cb = NULL` for a write operation on Windows
- **THEN** the write completes synchronously and returns `xErrno_Ok`

#### Scenario: Async write on Windows

- **WHEN** `xFsReqSubmit` is called with `cb != NULL` for a write operation on Windows
- **THEN** the operation returns `xErrno_Pending`
- **AND** the callback is invoked on the event loop thread with `xErrno_Ok` after the write completes

#### Scenario: Async read on Windows

- **WHEN** `xFsReqSubmit` is called with `cb != NULL` for a read operation on Windows
- **THEN** the data is read from the file at the specified offset and delivered via the callback

#### Scenario: File stat on Windows

- **WHEN** `xFsReqSubmit` is called with `xFsOpStat` on Windows
- **THEN** `r->stat.size` reflects the file size and `r->stat.mode` indicates regular file or directory

### Requirement: Backend selection at compile time

On Windows targets, the `fs_win32.c` source SHALL be compiled instead of `fs.c`. No runtime dispatch is needed. Existing POSIX behavior on macOS/Linux SHALL remain unchanged.

#### Scenario: Windows compilation selects win32 backend

- **WHEN** the project is built on Windows (MSVC or MinGW)
- **THEN** `fs_win32.c` is compiled and `fs.c` is excluded

#### Scenario: POSIX compilation selects POSIX backend

- **WHEN** the project is built on macOS or Linux
- **THEN** `fs.c` is compiled and `fs_win32.c` is excluded
