# libxpp

C++17 bindings for libx — smart pointers, async primitives, and type utilities.

## Modules

- [EventLoop & WaitScope](event.md) — RAII wrappers for the libx event loop
- [Promise\<T\>](promise.md) — Composable deferred values (Rust Future trait in C++17)
- [Timer](timer.md) — Callback-based timer with pause/resume
- [Result\<T, E\>](result.md) — Success or error (Rust Result)
- [Option\<T\>](option.md) — A value or nothing (Rust Option)
- [Smart Pointers](smart-pointers/README.md) — Own, Box, Rc/Arc, NonNull
- [Variant](variant.md) — Type-safe tagged union
- [Panic](panic.md) — Assert macros
- [Compiler Macros](compiler.md) — Attribute/deprecation helpers
- [Opaque Handle Wrapper](opaque.md) — XDEF_HANDLE macro
