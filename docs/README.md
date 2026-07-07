# The Story of x

## Why This Project Exists

C++11 introduced rvalue references, and with them came **move semantics** — a mechanism that at first glance seems like a performance optimization (avoid copying large objects) but is actually something far more profound. Before move semantics, C++ had two ways to pass a value: copy it, or pass a pointer. Copying is safe but expensive; pointers are cheap but dangerous — nothing in the type system tells you who owns the pointed-to memory, when it will be freed, or whether it's even still valid. This is why C++ codebases are haunted by "who frees this?" and "is this pointer still alive?" — questions that don't exist in garbage-collected languages, and that C++ developers pay for with valgrind sessions, ASan runs, and late-night debugging.

Move semantics bridges this gap. When you `std::move` a value, you're not copying bits — you're transferring *ownership*. The source object is left in a valid-but-unspecified state, and the destination assumes full responsibility. This is **ownership semantics**, not just a copy elision trick. Combined with RAII destructors, move semantics lets you express in the type system: "I am the only one who holds this resource, and when I go out of scope, it gets cleaned up." No reference counting, no garbage collector, no manual `free`.

Rust took this idea and made it the foundation of the language — every value has exactly one owner, the compiler enforces borrowing rules at compile time, and you get memory safety without a runtime. C++ can't match Rust's compiler-level guarantees, but we can get surprisingly close with a library. That's where **libxpp** comes in: a C++11 wrapper that uses move semantics to implement `Own<T>` (single-owner heap allocation), `Box<T>`, `Rc<T>` / `Arc<T>` (shared ownership), and a family of Rust-inspired types like `Option<T>`, `Result<T, E>`, and `Variant<Ts...>`. The goal is to make value semantics, especially move semantics, the default way you write C++ — so your code reads like "I have a value, I move it to you" rather than "here's a pointer, please don't forget to free it."

## Beyond Smart Pointers: The Async Problem

Solid value types are necessary but not sufficient. A modern programming language also needs a good story for **asynchronous I/O**. The C/C++ ecosystem has no shortage of event libraries — libevent, libev, and my personal favorite, libuv — but these are *event notification* libraries, not *async programming* frameworks. They tell you that a socket became readable, but they don't give you the experience you get in Go or Rust: writing sequential-looking code that suspends and resumes across I/O boundaries.

To go from "the event loop told me there's data" to "I wrote `co_await socket.read(buf)` and it just worked" requires building: a scheduler that can park and resume tasks, a mechanism for coroutines to yield and be re-polled, a standard API for chaining async operations, combinators for running tasks concurrently or racing them against timeouts, integration with the event loop's I/O multiplexing, and — crucially — a way to propagate errors through the async chain without losing type information. This is the "非常多的东西" that separates a raw event library from an async runtime.

## Why Stackless Coroutines

There are two schools of thought for async programming at the runtime level:

**Stackful coroutines** (Go, Lua, libco): Each coroutine has its own stack, which means you can suspend at any depth of the call stack without changing any function signature. The runtime swaps stacks when a coroutine blocks. This is ergonomic — any function can be async without being marked — but comes at a cost: each coroutine needs a pre-allocated stack (typically 2–8 KB), context switching involves saving/restoring registers, and the runtime needs to manage stack growth.

**Stackless coroutines** (Rust, JavaScript, C++20): A coroutine is compiled into a state machine by the compiler. Each `co_await` point becomes a state transition. No separate stack is allocated — the coroutine's local variables become fields of an anonymous struct, and the whole frame is heap-allocated (or elided if the compiler can prove it's not needed). The cost per coroutine is roughly the size of its local variables plus a function pointer table. No context switching, no stack overflow risks, no runtime memory management beyond what the allocator already does.

We chose stackless for two reasons. First, C++20 standardized stackless coroutines with `co_await`/`co_return`, giving us a compiler-supported code generation path that is both portable and optimizable. Second, stackless coroutines compose better with zero-cost abstractions — the compiler can inline across coroutine boundaries, dead-code eliminate unused state variables, and allocate coroutine frames with custom allocators. A stackful approach would require either assembly-level stack manipulation (non-portable) or a separate threading model (heavyweight).

## The Promise<T> Abstraction

Rust's `Future` trait is the blueprint: an async operation is a state machine that, when polled, either returns `Poll::Ready(value)` or `Poll::Pending` and registers a waker to be called when progress can be made. The executor drives the state machine by calling `poll()` in a loop until the future completes.

C++ doesn't have this trait as a language feature, but it gives us the tools to build it. Our `Promise<T>` is a type-erased state machine with exactly this interface: `poll(waker) → Option<T>`. When a coroutine hits `co_await`, it suspends and registers its waker with the awaited sub-promise. When that sub-promise resolves, it calls the waker, which queues the suspended coroutine for re-polling on the event loop. This is the same core mechanism that powers `tokio`, just implemented at the library level rather than in the language runtime. The full design is documented in the [Promise chapter](libxpp/promise/README.md).

## Building the Async Stack

With `Promise<T>` as our foundation, we can build the same async modules that Rust developers reach for:

- **I/O utilities**: `BufReader`/`BufWriter` for efficient buffered I/O, `io::copy()` and `io::read_all()` for common patterns, `Duplex`/`Simplex` for in-process communication
- **Network**: `TcpStream` for async TCP, `TcpListener` for accepting connections, `UdpSocket`, DNS resolution, TLS with OpenSSL or mbedTLS
- **FileSystem**: async `File` with cursor tracking, `stat`, directory operations
- **Channels**: `oneshot` (single-value), `mpsc` (bounded and unbounded), `broadcast` (multi-consumer with lag detection), `watch` (version-tracked latest value), plus `Notify` for bare signal coordination

We deliberately align our API with both the STL (naming conventions, iterator patterns) and Tokio (channel semantics, async method signatures, error types). A Rust developer should recognize `mpsc::channel::<T>(cap)`; a C++ developer should find `rx.recv()` and `tx.send(v)` familiar. This dual alignment is a design constraint, not an afterthought.

## The Foundation: libx

All of this runs on top of **libx**, a C99 library that provides the event loop, non-blocking I/O, timers, and the lock-free MPSC queue used by our channel implementations. libx was built with the same philosophy: give C developers an async runtime they can start using immediately — no callback hell, no manual fd management, just `xTcpConnect()` and a promise-like callback. libxpp is the C++ layer that adds type safety, move semantics, and coroutine ergonomics on top.

---

The result is a stack where C and C++ each get the async experience they deserve: libx for systems programmers who need raw control with structured concurrency, and libxpp for application developers who want to write `co_await tcp_stream.read(buf)` and have it just work — no pointers, no leaky abstractions, no callback pyramids.
