# libxpp

C++11 bindings for libx — smart pointers, async primitives, and type utilities. Header-only.

## Modules

- [EventLoop & WaitScope](event.md) — RAII wrappers for the libx event loop
- [Promise](promise/README.md) — Composable deferred values (Rust Future trait in C++)
  - [Deferred Resolution](promise/deferred.md) — `async()`, `PromiseResolver`, cross-thread
  - [Timers & Timeouts](promise/timers.md) — `after()`, timeout pattern
  - [Combinators (all/race)](promise/combinators.md) — `all()`, `race()`, waker sharing
  - [Custom Adapters](promise/adapter.md) — `adapt`, `work`, Adapter contract
  - [C++20 Coroutines](promise/coroutine.md) — `co_await`/`co_return`
  - [Internals](promise/internals.md) — PromiseNode hierarchy, poll-based model
- [Allocator](allocator.md) — Allocator protocol, GlobalAllocator, custom allocators
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
- [Panic](panic.md) — Assert macros
- [Compiler Macros](compiler.md) — Attribute/deprecation helpers
- [Opaque Handle Wrapper](opaque.md) — RAII for `XDEF_HANDLE` typedefs
