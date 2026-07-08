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

Move semantics bridges this gap. When you `std::move` a value, you're not copying bits — you're transferring **ownership**. The source object is left in a valid-but-unspecified state, and the destination assumes full responsibility. Combined with RAII destructors, move semantics lets you express in the type system: "I am the only one who holds this resource, and when I go out of scope, it gets cleaned up." No reference counting, no garbage collector, no manual `free`.

Rust took this idea and made it the foundation of the language — every value has exactly one owner, the compiler enforces borrowing rules at compile time, and you get memory safety without a runtime. C++ can't match Rust's compiler-level guarantees, but we can get surprisingly close with a library. That's where **libxpp** comes in: a C++11 wrapper that uses move semantics to implement `Own<T>` (single-owner heap allocation), `Box<T>`, `Rc<T>` / `Arc<T>` (shared ownership), `NonNull<T>` (non-null pointer abstraction), and a family of Rust-inspired types like `Option<T>`, `Result<T, E>`, and `Variant<Ts...>`. The goal is to make value semantics, especially move semantics, the default way you write C++ — so your code reads like "I have a value, I move it to you" rather than "here's a pointer, please don't forget to free it."

## Beyond Smart Pointers

Solid value types are necessary but not sufficient. A modern language also needs a good story for **asynchronous I/O**. The C/C++ ecosystem has no shortage of event libraries — libevent, libev, and my personal favorite, libuv — but these are *event notification* libraries, not *async programming* frameworks. They tell you that a socket became readable, but they don't give you the experience you get in Go or Rust: writing sequential-looking code that suspends and resumes across I/O boundaries.

To go from "the event loop told me there's data" to "I wrote `.await()` and it just worked" requires building: a scheduler that can park and resume tasks, a mechanism to yield and be re-polled, a standard API for chaining async operations, combinators for running tasks concurrently or racing them against timeouts, integration with the event loop's I/O multiplexing, and — crucially — a way to propagate errors through the async chain without losing type information. This is the sheer amount of infrastructure that separates a raw event library from an async runtime.

A natural question: why not just use Boost.Asio? Asio is the most mature async library in the C++ ecosystem, but it was designed before C++11 was widespread. It's built on a callback chain and `io_service` scheduling model where the type system plays a minimal role and error handling is almost entirely `error_code`. Coroutine support was retrofitted with macros and templates — it wasn't designed in from the start. We wanted an async stack where type safety, move semantics, and multiple await styles are first-class citizens from day one.

## The Promise\<T\> Abstraction

Rust's `Future` trait is the blueprint: an async operation is a state machine that, when polled, either returns `Poll::Ready(value)` or `Poll::Pending` and registers a waker to be called when progress can be made. The executor drives the state machine by calling `poll()` in a loop until the future completes.

C++ doesn't have this trait as a language feature, but it gives us the tools to build it. Our `Promise<T>` is a concrete template with the polling interface `poll(waker) → Option<T>` — it holds a type-erased node (coroutine frame, adapter, or chain), but from the user's perspective, `Promise<T>` is always fully typed and the compiler checks every call site.

When you call `.await()` on a Promise, it enters a polling loop: try `poll()`, and if the value isn't ready, park the current context so the event loop can make progress. Once the Promise resolves — an I/O completes, a timer fires, a channel receives — the waker fires, the polling loop un-parks, and `poll()` returns the value. This is the same core mechanism that powers `tokio`, just implemented at the library level rather than in the language runtime. The full design is documented in the [Promise chapter](libxpp/promise/).

## How to Use It

Because everything converges on `poll()`, `Promise<T>` supports three coding styles — each equally valid, each using the same underlying machinery:

### 1. `.await()` — any C++11 compiler

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

int result = fetch_value()              // Promise<int>
    .then([](int x) { return x * 2; })
    .await();                           // runs event loop until resolved
```

`.await()` *drives the event loop itself* — it calls `xEventLoopRun(X_RUN_ONCE)` in a loop until the Promise resolves. This is the universal entry point: it works in `main()`, in tests, anywhere a `WaitScope` is active.

### 2. `.await()` + fiber — non-blocking concurrency

```cpp
xpp::fiber([]() {
    auto a = http_get("/a").await();    // fiber suspends, event loop continues
    auto b = http_get("/b").await();    // resumes when a is ready
    return a + b;
}).then([](int total) {
    printf("total = %d\n", total);
});
```

Wrap your code in `xpp::fiber()` and `.await()` *automatically* becomes non-blocking. The fiber gets its own 64 KiB `mmap`'d stack with a guard page. When `.await()` needs to wait, it calls `swapcontext` to switch back to the event loop — the fiber freezes in place, and the thread can run other fibers or handle I/O. When the Promise resolves, the waker switches back to the fiber exactly where it left off.

This is the headline feature of libxpp: **C++11, no `co_await` syntax, no colored functions, no compiler support needed.** You get the same linear-code experience as Rust's `.await` or Go's goroutines, on any C++11 toolchain.

### 3. `co_await` / `co_return` — C++20 coroutines

```cpp
xpp::Promise<int> compute() {
    int x = co_await fetch_value();
    co_return x * 2;
}
```

If you have a C++20 compiler, `Promise<T>` is directly a coroutine return type. `co_await` compiles into the same `poll()` / waker mechanism as `.then()` chains — no separate runtime, no `Task<T>` wrapper.

All three styles interoperate freely. A `Promise<T>` returned by `.then()` can be `.await()`'d in a fiber, a coroutine can `co_await` a Promise built from a callback chain, and `.then()` can append a callback to a Promise returned by a coroutine. The library doesn't care which style you choose — it's the same `poll()` underneath.

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
- **[Promise Model](libxpp/promise/)** — the poll-and-waker state machine, `.await()` semantics (fiber suspend + direct event loop drive), C++20 coroutine frame mapping, and the internals of chaining, cancellation, and error propagation.
- **[Async I/O](libxpp/io/)** — the layering from raw `AsyncFd` up through `BufReader`/`BufWriter` to type-safe `TcpStream` and `File`, plus utilities like `io::copy` and in-process `Duplex`/`Simplex` pipes.
- **[Channels](libxpp/channels/)** — the full Tokio-aligned suite: `oneshot`, `mpsc` (bounded via lock-free ring buffer, unbounded via lock-free linked list), `broadcast` with lag recovery, `watch` with version-tracked "seen" semantics, and `Notify` as a reusable wake primitive.
- **[Threading Model](libxpp/promise/#thread-safety)** — the `XPP_MT` compile flag that switches `Shared<T>` from `Rc` to `Arc`, the `loom` module of swappable primitives for future concurrency testing, and RAII close semantics across all channels.
- **[Network](libxpp/net/)** — async TCP, UDP, DNS, and TLS, all built on the same `Promise<T>` foundation.
- **[Filesystem](libxpp/fs.md)** — async file I/O with cursor tracking, `stat`, and directory operations.
- **[Time](libxpp/time.md)(TODO)** — tokio-style time primitives — `Instant`, `Duration`, `sleep`, `interval`, `timeout` — built on `Promise<T>`.

The design philosophy throughout is the same: leverage what C++ gives us (move semantics, RAII, coroutine code generation) to build an async experience that feels like Rust with Tokio, still runs on a C foundation, and fits into existing C++ codebases without requiring a language fork or a custom compiler.

---

The result is a stack where C and C++ each get the async experience they deserve: libx for systems programmers who need raw control with structured concurrency, and libxpp for application developers who can choose between `xpp::fiber([]() { auto v = promise.await(); ... })` in C++11, `Promise<T>::then([](auto v){...})` for callback chains, or `co_await promise` for C++20 coroutines — no pointers, no leaky abstractions, no callback pyramids.
