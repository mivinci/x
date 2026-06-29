## Why

xfs (async filesystem module) currently uses POSIX APIs (`pread`, `pwrite`, `fcntl.h`, `unistd.h`) exclusively and is guarded with `#if !defined(_WIN32)` in tests. The dlproxy cache module depends on xfs for async I/O, which means dlproxy unit tests are also restricted to POSIX. Adding a Windows backend to xfs unblocks the entire async I/O stack on Windows.

## What Changes

- Add `fs_win32.c` backend using Win32 API (`ReadFile`/`WriteFile` with `OVERLAPPED`)
- Add `cmake/DetectWin32.cmake` detection logic to select the Windows backend
- Update `fs.h` to declare backend selection macros (`X_HAS_FS_WIN32`)
- Remove `#if !defined(_WIN32)` guards from `fs_test.cpp` and `cache_test.cpp` once xfs supports Windows
- No API changes — `xFsReqSubmit`/`xFsReqCancel` signature and behavior remain identical

## Capabilities

### New Capabilities

- `xfs-win32-backend`: Implement async filesystem I/O on Windows using the Win32 API (`ReadFile`/`WriteFile` with `OVERLAPPED` structs). The backend follows the same pattern as the existing POSIX backend (`fs.c`) — a `fs_win32.c` source file selected via CMake detection. Synchronous and async (thread-pool offloaded) modes are both supported, matching the POSIX behavior.

### Modified Capabilities

<!-- None — the existing xfs spec is unchanged; this adds a new backend without changing POSIX behavior. -->

## Impact

- `libx/x/fs/fs_win32.c` — new file, Win32 backend implementation
- `libx/x/fs/CMakeLists.txt` — add Win32 source file selection
- `libx/x/fs/fs_test.cpp` — remove `#if !defined(_WIN32)` guards
- `libdlproxy/dlproxy/cache_test.cpp` — remove `#if !defined(_WIN32)` guards
- No changes to xfs public API (`fs.h`)
