## ADDED Requirements

### Requirement: File open and create
`File::open(path)` opens an existing file read-only. `File::create(path, mode)` creates or truncates a file write-only. `File::open_with(path, flags, mode)` opens with custom flags. All return `Promise<File>`.

#### Scenario: Open existing file
- **WHEN** `File::open("data.txt")` is called and the file exists
- **THEN** the returned Promise resolves to a `File` object with a valid handle

#### Scenario: Open non-existent file
- **WHEN** `File::open("nonexistent.txt")` is called and the file does not exist
- **THEN** the returned Promise resolves to a `File` with `raw_fd() < 0` (error indicated by negative fd)

#### Scenario: Create new file
- **WHEN** `File::create("output.txt", 0644)` is called
- **THEN** the file is created (or truncated if exists) and the Promise resolves to a writable `File`

### Requirement: Async read
`file.read(buf, len, offset)` reads up to `len` bytes at `offset` into `buf`. Returns `Promise<ssize_t>` — bytes read (>= 0) or negative error.

#### Scenario: Read from file
- **WHEN** `file.read(buf, 1024, 0)` is called on an open file with content
- **THEN** the Promise resolves to the number of bytes read, and `buf` contains the data

#### Scenario: Read at EOF
- **WHEN** `file.read(buf, 1024, file_size)` is called at end of file
- **THEN** the Promise resolves to 0

#### Scenario: Read with Span overload
- **WHEN** `file.read(Span<uint8_t>(buf, 1024), 0)` is called
- **THEN** behavior is identical to `file.read(buf, 1024, 0)`

### Requirement: Async write
`file.write(buf, len, offset)` writes `len` bytes from `buf` at `offset`. Returns `Promise<ssize_t>` — bytes written (>= 0) or negative error.

#### Scenario: Write to file
- **WHEN** `file.write("hello", 5, 0)` is called on a writable file
- **THEN** the Promise resolves to 5, and the file contains "hello" at offset 0

#### Scenario: Write with string overload
- **WHEN** `file.write(std::string("hello"), 0)` is called
- **THEN** behavior is identical to `file.write("hello", 5, 0)`

### Requirement: Async close
`file.close()` closes the file handle asynchronously. Returns `Promise<void>`.

#### Scenario: Close open file
- **WHEN** `file.close()` is called on an open file
- **THEN** the Promise resolves after the file handle is closed, and `file.is_open()` returns false

### Requirement: RAII destructor
`~File()` closes the file synchronously if still open.

#### Scenario: Destructor closes file
- **WHEN** a `File` with an open handle is destroyed
- **THEN** the file handle is closed synchronously (blocking), no resource leak

#### Scenario: Destructor on already-closed file
- **WHEN** a `File` that was already closed (via `close()`) is destroyed
- **THEN** no double-close occurs

### Requirement: File metadata
`file.stat()` returns `Promise<Stat>` with file metadata. Free function `fs::stat(path)` stats by path.

#### Scenario: Stat open file
- **WHEN** `file.stat()` is called on an open file
- **THEN** the Promise resolves to a `Stat` struct with correct size, mode, mtime

#### Scenario: Stat by path
- **WHEN** `fs::stat("data.txt")` is called
- **THEN** the Promise resolves to a `Stat` struct

### Requirement: Sync to disk
`file.sync_all()` flushes file data and metadata to disk. Returns `Promise<void>`.

#### Scenario: Sync after write
- **WHEN** `file.sync_all()` is called after writing
- **THEN** the Promise resolves after data is persisted to disk

### Requirement: Convenience methods
`read_all()`, `read_to_string()`, `write_all()` provide one-shot operations.

#### Scenario: Read entire file
- **WHEN** `file.read_all()` is called on a file with 100 bytes
- **THEN** the Promise resolves to a `vector<uint8_t>` of size 100 containing the file data

#### Scenario: Read as string
- **WHEN** `file.read_to_string()` is called on a text file
- **THEN** the Promise resolves to a `std::string` with the file content

#### Scenario: Write all bytes
- **WHEN** `file.write_all(buf, len)` is called
- **THEN** the Promise resolves after all `len` bytes are written (may require multiple write calls internally)

### Requirement: Free functions
`fs::read(path)`, `fs::write(path, buf, len)`, `fs::stat(path)` provide file operations without managing a `File` object.

#### Scenario: Read file by path
- **WHEN** `fs::read("data.txt")` is called
- **THEN** the Promise resolves to a `vector<uint8_t>` with the file content

#### Scenario: Write file by path
- **WHEN** `fs::write("out.txt", buf, len)` is called
- **THEN** the file is created/truncated, written, and closed; Promise resolves to void

### Requirement: Raw handle access
`file.raw_fd()` returns the OS file descriptor. `file.is_open()` checks if the handle is valid.

#### Scenario: Get file descriptor
- **WHEN** `file.raw_fd()` is called on an open file
- **THEN** a valid file descriptor (>= 0) is returned

#### Scenario: From raw fd
- **WHEN** `File::from_raw_fd(fd)` is called with a valid fd
- **THEN** a `File` object is created that takes ownership of the fd
