## 1. FsAdapter infrastructure (fs/file.h)

- [x] 1.1 Define `xpp::fs::Stat` struct (maps `xFsStat`: size, mode, mtime, ctime)
- [x] 1.2 Define `FsAdapterBase` — owns `xFsReq`, submits on construct, cancels on destruct if !done
- [x] 1.3 Implement FsAdapter callback: extract result from `xFsReq`, resolve PromiseResolver, set done flag
- [x] 1.4 Specialize FsAdapter for each op type: FsOpenAdapter, FsReadAdapter, FsWriteAdapter, FsCloseAdapter, FsStatAdapter

## 2. File class (fs/file.h)

- [x] 2.1 Define `xpp::fs::File` class: `int m_fd`, `bool m_open`
- [x] 2.2 `static Promise<File> open(const char* path)` — O_RDONLY
- [x] 2.3 `static Promise<File> create(const char* path, int mode = 0644)` — O_WRONLY|O_CREAT|O_TRUNC
- [x] 2.4 `static Promise<File> open(const char* path, int flags, int mode = 0644)` — custom flags (merged open_with into overload)
- [x] 2.5 `static File from_raw_fd(int fd)` — construct from existing fd
- [x] 2.6 `Promise<ssize_t> read(void* buf, size_t len, off_t offset)` — base overload
- [x] 2.7 `Promise<ssize_t> read(Span<uint8_t> buf, off_t offset)` — Span overload
- [x] 2.8 `Promise<ssize_t> write(const void* buf, size_t len, off_t offset)` — base
- [x] 2.9 `Promise<ssize_t> write(Span<const uint8_t> buf, off_t offset)` — Span
- [x] 2.10 `Promise<ssize_t> write(const std::string& s, off_t offset)` — string
- [x] 2.11 `Promise<void> close()` — async close, sets m_open=false
- [x] 2.12 `Promise<void> sync_all()` — fsync
- [x] 2.13 `Promise<Stat> stat()` — fstat on open file
- [x] 2.14 `int raw_fd() const`, `bool is_open() const`
- [x] 2.15 `~File()` — RAII sync close if m_open

## 3. Convenience methods (fs/file.h)

- [x] 3.1 `Promise<std::vector<uint8_t>> read_all()` — stat → allocate → read
- [x] 3.2 `Promise<std::string> read_to_string()` — read_all → string
- [x] 3.3 `Promise<void> write_all(const void* buf, size_t len)` — loop write from offset 0

## 4. Free functions (fs/file.h)

- [x] 4.1 `Promise<Stat> stat(const char* path)` — stat by path
- [x] 4.2 `Promise<std::vector<uint8_t>> read(const char* path)` — open → read_all → close
- [x] 4.3 `Promise<void> write(const char* path, const void* buf, size_t len)` — open(create) → write_all → close

## 5. Tests (fs/file_test.cpp)

- [x] 5.1 Open existing file, read content, verify
- [x] 5.2 Open non-existent file, verify error (negative fd)
- [x] 5.3 Create file, write content, close, reopen, verify
- [x] 5.4 Read at EOF, verify 0 bytes
- [x] 5.5 Write with string overload, verify
- [x] 5.6 Read with Span overload, verify
- [x] 5.7 RAII: destroy File without close(), verify no leak (reopen and read)
- [x] 5.8 stat() on open file and by path, verify size/mode
- [x] 5.9 sync_all() after write, verify no error
- [x] 5.10 read_all() convenience, verify full content
- [x] 5.11 read_to_string() convenience, verify string content
- [x] 5.12 write_all() convenience, verify all bytes written
- [x] 5.13 Free function read(path), verify content
- [x] 5.14 Free function write(path, buf, len), verify content
- [x] 5.15 from_raw_fd(), verify operations work
- [ ] 5.16 Coroutine: co_await file.read() in a coroutine

## 6. Docs

- [ ] 6.1 Create `docs/libxpp/fs.md` — File API, examples, comparison with tokio::fs::File
- [ ] 6.2 Update `docs/SUMMARY.md` — add fs page
- [ ] 6.3 Update `docs/libxpp/README.md` — add fs to module list
