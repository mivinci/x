## ADDED Requirements

### Requirement: AsyncFd registration and readiness
`AsyncFd` registers a non-blocking fd with the event loop once (edge-triggered, Read|Write). Tracks readiness via internal bools. Event callback sets readiness and resolves pending waiter.

#### Scenario: Create and destroy
- **WHEN** `AsyncFd fd(sock_fd)` is constructed
- **THEN** the fd is registered with the current event loop for Read|Write events

#### Scenario: Destroy deregisters
- **WHEN** an `AsyncFd` is destroyed
- **THEN** the event source is deregistered via `xEventDel`, and pending waiters are resolved (EOF)

### Requirement: readable() returns Promise
`readable()` returns `Promise<void>` that resolves when the fd becomes readable. If already readable, resolves immediately.

#### Scenario: Already readable
- **WHEN** `readable()` is called and the fd is already readable (readiness bit set)
- **THEN** the Promise resolves immediately, no event registration needed

#### Scenario: Not readable
- **WHEN** `readable()` is called and the fd is not readable
- **THEN** the PromiseResolver is stored in the AsyncFd, and the Promise resolves when `on_event` fires with `xEvent_Read`

#### Scenario: Promise destroyed before event
- **WHEN** the Promise returned by `readable()` is destroyed before the event fires
- **THEN** `on_event` calls `resolve()` on the stored PromiseResolver, which is a no-op (ArcWeak upgrade fails). No use-after-free.

### Requirement: writable() returns Promise
`writable()` returns `Promise<void>` that resolves when the fd becomes writable. Same semantics as `readable()`.

### Requirement: read() with fast path
`io::read(fd, buf, len)` tries `recv()` immediately. If data available, resolves with byte count. If EAGAIN, chains `readable().then(recv)`.

#### Scenario: Data available
- **WHEN** `read(fd, buf, len)` is called and data is available
- **THEN** the Promise resolves immediately with bytes read, zero Promise chain overhead

#### Scenario: EAGAIN
- **WHEN** `read(fd, buf, len)` is called and recv returns EAGAIN
- **THEN** the Promise chains `readable().then(retry recv)`, resolving when the fd becomes readable

#### Scenario: Error
- **WHEN** `read(fd, buf, len)` is called and recv returns an error (not EAGAIN)
- **THEN** the Promise resolves with the negative errno value

### Requirement: write() with fast path
`io::write(fd, buf, len)` tries `send()` immediately. Same fast/slow path pattern as `read()`.

### Requirement: read_full() loops until complete
`io::read_full(fd, buf, len)` reads exactly `len` bytes, looping `read()` until the buffer is full or EOF/error.

#### Scenario: Read exact bytes
- **WHEN** `read_full(fd, buf, 1024)` is called on a file with 1024+ bytes
- **THEN** the Promise resolves after reading exactly 1024 bytes

#### Scenario: EOF before full
- **WHEN** `read_full(fd, buf, 1024)` is called on a file with 500 bytes
- **THEN** the Promise resolves with 500 (bytes actually read)

### Requirement: write_all() loops until complete
`io::write_all(fd, buf, len)` writes all `len` bytes, looping `write()` until complete or error.

#### Scenario: Write all bytes
- **WHEN** `write_all(fd, buf, 1024)` is called
- **THEN** the Promise resolves after all 1024 bytes are written

### Requirement: close() wakes waiters
`AsyncFd::close()` deregisters from event loop and wakes both read and write waiters.

#### Scenario: Close with pending readers
- **WHEN** `close()` is called while a `readable()` Promise is pending
- **THEN** the pending Promise resolves (as if EOF), and the event source is deregistered

### Requirement: fd accessors
`fd()` returns the raw fd. `is_closed()` returns true after `close()`.

#### Scenario: Get fd
- **WHEN** `fd()` is called on an open AsyncFd
- **THEN** the raw file descriptor is returned

### Requirement: AsyncFd is movable, not copyable
AsyncFd is move-only. Moved-from state is a tombstone (fd == -1, no event source).

#### Scenario: Move construct
- **WHEN** `AsyncFd b(std::move(a))` is called
- **THEN** `b` owns the event source, `a` is a tombstone (fd == -1, is_closed() == true)
