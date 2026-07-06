# libxpp

C++11 bindings for libx — smart pointers, async primitives, and type utilities. Header-only.

## At a Glance

```cpp
#include <xpp/arc.h>
#include <xpp/box.h>
#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/promise.h>

// Result<T, E> — explicit error handling, no exceptions
xpp::Result<int, std::string> parse(std::string_view s) {
    if (s.empty()) return xpp::err("empty input");
    return xpp::ok(std::stoi(std::string(s)));
}

// Arc<T> — thread-safe shared ownership, sizeof == sizeof(T*)
// (std::shared_ptr is 2× ptr; Arc is 1× ptr with niche-optimized Option)
auto config = xpp::Arc<Config>::make(args...);

// Promise<T> — poll-based async, single-threaded executor
// (like Tokio's current_thread; C++20 coroutines optional)
xpp::Promise<Stats> fetch() {
    auto raw = co_await http_get("/api/stats");
    co_return parse_stats(raw);
}

// Option<T> — nullptr == None, sizeof == sizeof(T*)
xpp::Option<xpp::Arc<Config>> cached = lookup(key);
if (cached) {
    use(**cached);     // Option → Arc → Config
}
```

**Design philosophy:**

- **Rust-inspired, C++11-compatible** — `Result`/`Option`/`Arc`/`Box` with the same semantics as their Rust counterparts, but portable to any C++11 toolchain.
- **Zero overhead** — every smart pointer is `sizeof(T*)`. `Option<Arc<T>>` is also `sizeof(T*)` via niche optimization (`nullptr = None`). Empty allocators vanish via EBO.
- **Poll-based Promise** — single-threaded executor (like Tokio's `current_thread`). `wait()` drives the event loop on the calling thread; `co_await` is optional (C++20).
- **Single allocation** — `Arc::make()` allocates the control block and value together in one heap block, matching Rust's `Arc::new`.

## Modules

- [EventLoop & WaitScope](event.md) — RAII wrappers for the libx event loop
- [Promise](promise/README.md) — Composable deferred values (Rust Future trait in C++)
  - [Deferred Resolution](promise/deferred.md) — `async()`, `PromiseResolver`, cross-thread
  - [Timers & Timeouts](promise/timers.md) — `after()`, timeout pattern
  - [Combinators (all/race)](promise/combinators.md) — `all()`, `race()`, waker sharing
  - [Utilities (try\_next)](promise/utils.md) — `try_next()`, sequential fall-through
  - [Custom Adapters](promise/adapter.md) — `adapt`, `work`, Adapter contract
  - [C++20 Coroutines](promise/coroutine.md) — `co_await`/`co_return`
  - [Internals](promise/internals.md) — PromiseNode hierarchy, poll-based model
- [Allocator](allocator.md) — Allocator protocol, GlobalAllocator, custom allocators
- [Arena](arena.md) — Bump allocator for short-lived objects (`Arena<N>`)
- [Smart Pointers](smart-pointers/README.md) — `Own`, `Box`, `Rc`/`Weak`, `Arc`/`ArcWeak`, `NonNull`
  - [Own](smart-pointers/own.md) — Nullable unique ownership
  - [Box](smart-pointers/box.md) — Non-null unique ownership
  - [Rc & Weak](smart-pointers/rc.md) — Single-thread shared ownership
  - [Arc & ArcWeak](smart-pointers/arc.md) — Thread-safe shared ownership
  - [NonNull](smart-pointers/nonnull.md) — Non-owning, non-null reference
- [Result](result.md) — Success or error (Rust Result)
- [Option](option.md) — A value or nothing (Rust Option)
- [Variant](variant.md) — Type-safe tagged union
- [Timer](timer.md) — Callback-based timer with pause/resume
- [Filesystem](fs.md) — Async file I/O (`File`, `stat`, `exists`, `create_dir`, `rename`)
- [I/O](io/README.md) — Reactive async I/O for non-blocking fds (`AsyncFd`, `read`, `write`, `Error`)
- [Net](net/README.md) — Async TCP/UDP/DNS/URL/TLS (`TcpStream`, `TcpListener`, `UdpSocket`, `lookup_host`, `Url`, `TlsContext`)
- [Panic](panic.md) — Assert macros
- [Compiler Macros](compiler.md) — Attribute/deprecation helpers
- [Opaque Handle Wrapper](handle.md) — RAII for `XDEF_HANDLE` typedefs
