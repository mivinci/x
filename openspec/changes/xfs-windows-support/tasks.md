## 1. Windows Backend Implementation

- [x] 1.1 Create `fs_win32.c` — map all 8 xFsOp operations to CRT APIs (`_open`, `_read`/`_lseeki64`, `_write`/`_lseeki64`, `_close`, `_stat64`, `_mkdir`, `_unlink`, `rename`)
- [x] 1.2 Handle `xFsOpRead` and `xFsOpWrite` with manual offset management (seek → read/write → restore offset)
- [x] 1.3 Ensure async mode works via `xWorkSubmit` (same pattern as `fs.c`)
- [x] 1.4 Map POSIX file permissions to Windows equivalents (e.g., 0644 → `_S_IREAD | _S_IWRITE`)

## 2. CMake Integration

- [x] 2.1 Add `cmake/DetectWin32.cmake` or inline detection in `fs/CMakeLists.txt`
- [x] 2.2 On Windows: compile `fs_win32.c`, exclude `fs.c`
- [x] 2.3 On POSIX: compile `fs.c`, exclude `fs_win32.c`

## 3. Remove Windows Guards

- [x] 3.1 Remove `#if !defined(_WIN32)` from `fs_test.cpp` — tests should run on Windows now
- [x] 3.2 Remove `#if !defined(_WIN32)` from `cache_test.cpp`
- [x] 3.3 Verify `fs_test` and `cache_test` pass on macOS/Linux (no regression)
