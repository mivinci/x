# The Story of the X Project

[中文版](README_CN.md)

## Why This Project Exists

C++11 introduced rvalue references, and with them came **move semantics** — a mechanism that at first glance seems like a performance optimization (avoid copying large objects) but is actually something far more profound. Before move semantics, C++ had two ways to pass a value: copy it, or pass a pointer. Copying is safe but expensive; pointers are cheap but dangerous — nothing in the type system tells you who owns the pointed-to memory, when it will be freed, or whether it's even still valid. This is why C++ codebases are haunted by "who frees this?" and "is this pointer still alive?" — questions that don't exist in garbage-collected languages, and that C++ developers pay for with valgrind sessions, ASan runs, and late-night debugging.

```cpp
// Three ways to pass a buffer to another function:

// 1. Copy — safe, but deep-copies gigabytes of video frame data.
void process_copy(std::vector<uint8_t> buf);  // caller knows buf is copied

// 2. Raw pointer — cheap, but who owns this? Who frees it?
void process_ptr(uint8_t* data, size_t len);  // caller: "is data still valid?"

// 3. Rvalue reference — cheap AND clear. "I'm done with this, it's yours."
void process_move(std::vector<uint8_t>&& buf); // caller: std::move(buf)
                                               // callee: sole owner, RAII cleanup
```

Traditional C++ leans on copies (too expensive for large objects) or pointers
(too ambiguous for ownership). Neither encodes *who holds the value* into the
function signature.

Move semantics bridges this gap. When you `std::move` a value, you're not copying bits — you're transferring *ownership*. The source object is left in a valid-but-unspecified state, and the destination assumes full responsibility. This is **ownership semantics**, not just a copy elision trick. Combined with RAII destructors, move semantics lets you express in the type system: "I am the only one who holds this resource, and when I go out of scope, it gets cleaned up." No reference counting, no garbage collector, no manual `free`.

Rust took this idea and made it the foundation of the language — every value has exactly one owner, the compiler enforces borrowing rules at compile time, and you get memory safety without a runtime. C++ can't match Rust's compiler-level guarantees, but we can get surprisingly close with a library. That's where **libxpp** comes in: a C++11 wrapper that uses move semantics to implement `Own<T>` (single-owner heap allocation), `Box<T>`, `Rc<T>` / `Arc<T>` (shared ownership), `NonNull<T>` (non-null pointer abstraction), and a family of Rust-inspired types like `Option<T>`, `Result<T, E>`, and `Variant<Ts...>`. The goal is to make value semantics, especially move semantics, the default way you write C++ — so your code reads like "I have a value, I move it to you" rather than "here's a pointer, please don't forget to free it."

## Beyond Smart Pointers

Solid value types are necessary but not sufficient. A modern programming language also needs a good story for **asynchronous I/O**. The C/C++ ecosystem has no shortage of event libraries — libevent, libev, and my personal favorite, libuv — but these are *event notification* libraries, not *async programming* frameworks. They tell you that a socket became readable, but they don't give you the experience you get in Go or Rust: writing sequential-looking code that suspends and resumes across I/O boundaries.

To go from "the event loop told me there's data" to "I wrote `co_await socket.read(buf)` and it just worked" requires building: a scheduler that can park and resume tasks, a mechanism for coroutines to yield and be re-polled, a standard API for chaining async operations, combinators for running tasks concurrently or racing them against timeouts, integration with the event loop's I/O multiplexing, and — crucially — a way to propagate errors through the async chain without losing type information. This is the sheer amount of infrastructure that separates a raw event library from an async runtime.

A natural question: why not just use Boost.Asio? Asio is the most mature async library in the C++ ecosystem, but it was designed before C++11 was widespread. It's built on a callback chain and `io_service` scheduling model where the type system plays a minimal role and error handling is almost entirely `error_code`. Coroutine support was retrofitted with macros and templates — it wasn't designed in from the start. We wanted an async stack where type safety, move semantics, and coroutines are first-class citizens from day one.

## Stackless + Stackful: Two Coroutine Models

There are two schools of thought for async programming at the runtime level:

**Stackful coroutines** (Go, Lua, libco, `xpp::fiber()`): Each coroutine has its own stack (64 KiB, mmap'd with guard page). When a coroutine blocks on `.await()`, the runtime swaps to the event loop stack via `swapcontext` — no function signatures need to change, no compiler support needed, works in C++11. The coroutine's locals live on its own stack, so you can `.await()` at any call depth.

**Stackless coroutines** (Rust, JavaScript, C++20 `co_await`): A coroutine is compiled into a state machine by the compiler. Each `co_await` point becomes a state transition. No separate stack is allocated — the coroutine's local variables become fields of an anonymous struct, and the whole frame is heap-allocated (or elided if the compiler can prove it's not needed). The cost per coroutine is roughly the size of its local variables plus a function pointer table.

**libxpp supports both.** For C++11 codebases (or anywhere you prefer not to color functions with `co_await`), `xpp::fiber()` gives you stackful fibers with a dedicated stack — write linear `.await()` code, the fiber automatically suspends and resumes. For C++20 codebases, native `co_await` / `co_return` work directly on `Promise<T>`. Both converge on the same `poll()`-based `PromiseWaker` mechanism — the only difference is how execution is suspended and resumed.

## The Promise\<T\> Abstraction

Rust's `Future` trait is the blueprint: an async operation is a state machine that, when polled, either returns `Poll::Ready(value)` or `Poll::Pending` and registers a waker to be called when progress can be made. The executor drives the state machine by calling `poll()` in a loop until the future completes.

C++ doesn't have this trait as a language feature, but it gives us the tools to build it. Our `Promise<T>` is a concrete template with the polling interface `poll(waker) → Option<T>` — internally it holds a type-erased coroutine frame or adapter node, but from the user's perspective, `Promise<T>` is always fully typed and the compiler checks every call site. It works with C++11 (via `.then()` or `.await()`) and C++20 (via `co_await` / `co_return`). With `XPP_FIBER`, `.await()` automatically detects whether it's inside a fiber and uses stackful suspend — the same `.await()` call works both inside and outside `xpp::fiber()`. The full design is documented in the [Promise chapter](libxpp/promise/).

When a C++20 coroutine hits `co_await`, it suspends and registers its waker with the awaited sub-promise. When that sub-promise resolves, it calls the waker, which queues the suspended coroutine for re-polling on the event loop. This is the same core mechanism that powers `tokio`, just implemented at the library level rather than in the language runtime. The full design is documented in the [Promise chapter](libxpp/promise/).

## Building the Async Stack

With `Promise<T>` as our foundation, we can build the same async modules that Rust developers reach for:

- **I/O utilities**: `BufReader`/`BufWriter` for efficient buffered I/O, `io::copy()` and `io::read_all()` for common patterns, `Duplex`/`Simplex` for in-process communication
- **Network**: `TcpStream` for async TCP, `TcpListener` for accepting connections, `UdpSocket`, DNS resolution, TLS with OpenSSL or mbedTLS
- **FileSystem**: async `File` with cursor tracking, `stat`, directory operations
- **Channels**: `oneshot` (single-value), `mpsc` (bounded and unbounded), `broadcast` (multi-consumer with lag detection), `watch` (version-tracked latest value), plus `Notify` for bare signal coordination

We deliberately align our API with both the STL (naming conventions, iterator patterns) and Tokio (channel semantics, async method signatures, error types). A Rust developer should recognize `mpsc::channel::<T>(cap)`; a C++ developer should find `rx.recv()` and `tx.send(v)` familiar. This dual alignment is a design constraint, not an afterthought.

## The Foundation: libx

All of this runs on top of **libx**, a C99 library that provides the event loop, non-blocking I/O, timers, lock-free queue primitives, and (since `xbase/fiber.h`) cross-platform stackful fibers with `xFiberCreate` / `xFiberSwitch`. libx was built with the same philosophy: give C developers an async runtime they can start using immediately — no callback hell, no manual fd management, just `xTcpConnect()` and a promise-like callback. libxpp is the C++ layer that adds type safety, move semantics, `.await()` ergonomics, and `xpp::fiber()` integration on top — the name says it directly: the C core is **libx**, the C++ binding is **libxpp**.

## Into the Design

If you want to dive into the details behind each piece:

- **[Type System](libxpp/smart-pointers/)** — how `Own<T>`, `Box<T>`, `Rc<T>`, `Arc<T>`, and `NonNull<T>` implement Rust-style ownership in a library, and where the limits are compared to a compiler-enforced borrow checker.
- **[Promise Model](libxpp/promise/)** — the poll-and-waker state machine, how C++20 coroutine frames map to `Promise<T>`, and the internals of chaining, cancellation, and error propagation.
- **[Async I/O](libxpp/io/)** — the layering from raw `AsyncFd` up through `BufReader`/`BufWriter` to type-safe `TcpStream` and `File`, plus utilities like `io::copy` and in-process `Duplex`/`Simplex` pipes.
- **[Channels](libxpp/channels/)** — the full Tokio-aligned suite: `oneshot`, `mpsc` (bounded via lock-free ring buffer, unbounded via lock-free linked list), `broadcast` with lag recovery, `watch` with version-tracked "seen" semantics, and `Notify` as a reusable wake primitive.
- **[Threading Model](libxpp/promise/#thread-safety)** — the `XPP_MT` compile flag that switches `Shared<T>` from `Rc` to `Arc`, the `loom` module of swappable primitives for future concurrency testing, and RAII close semantics across all channels.
- **[Network](libxpp/net/)** — async TCP, UDP, DNS, and TLS, all built on the same `Promise<T>` foundation.
- **[Filesystem](libxpp/fs.md)** — async file I/O with cursor tracking, `stat`, and directory operations.
- **[Time](libxpp/time.md)(TODO)** — tokio-style time primitives — `Instant`, `Duration`, `sleep`, `interval`, `timeout` — built on `Promise<T>`.

The design philosophy throughout is the same: leverage what C++ gives us (move semantics, RAII, coroutine code generation) to build an async experience that feels like Rust with Tokio, still runs on a C foundation, and fits into existing C++ codebases without requiring a language fork or a custom compiler.

---

The result is a stack where C and C++ each get the async experience they deserve: libx for systems programmers who need raw control with structured concurrency, and libxpp for application developers who can choose between `xpp::fiber([]() { auto v = promise.await(); ... })` in C++11, `Promise<T>::then([](auto v){...})` for callback chains, or `co_await promise` for C++20 coroutines — no pointers, no leaky abstractions, no callback pyramids.
