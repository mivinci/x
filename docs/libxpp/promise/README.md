# Promise\<T\> — Composable Deferred Values

[← libxpp](../README.md)

## Introduction

`Promise<T>` provides a type-safe async programming system within the libx event loop. It combines the poll-based model from Rust's `Future` trait with the node-hierarchy and per-chain arena allocation from KJ (Cap'n Proto), plus stackful fiber support via `xpp::fiber()`.

The core API (`.then()`, `.await()`, `resolve()`, `all()`, `race()`) is C++11. C++20 is required only for `co_await` / `co_return`.

`Promise<T>` supports three equivalent coding styles — pick whichever fits your compiler and preference:

**C++11 + `.await()`** (works everywhere):

```cpp
xpp::Promise<int> compute() {
    return fetch_value()
        .then([](int x) { return x * 2; });
}
int result = compute().await();  // drives event loop if not in fiber
```

**C++11 + fiber** (non-blocking inside `xpp::fiber()`):

```cpp
int result = xpp::fiber([]() {
    int x = fetch_value().await();     // fiber suspends, event loop continues
    return x * 2;
}).await();
```

**C++20 `co_await` / `co_return`**:

```cpp
xpp::Promise<int> compute() {
    int x = co_await fetch_value();
    co_return x * 2;
}
int result = compute().await();
```

All three are backed by the same `poll()`-based state machine.

## Design Philosophy

1. **`.await()` First** — `.await()` is the universal entry point. Outside a fiber it drives `xEventLoopRun()` directly. Inside a fiber (via `xpp::fiber()`) it suspends the fiber via `xFiberYield()` — non-blocking, stackful concurrency without `co_await` syntax.
2. **One-Shot Polling** — `poll()` returns `Option<T>`: `Some(value)` = ready, `None` = pending. No separate `take()`.
3. **Single-Threaded Executor** — Like Tokio's `current_thread` runtime. The event loop is both reactor (I/O) and scheduler (timers/callbacks). No background thread pool needed.
4. **Auto-Flatten** — `.then(fn)` returning `Promise<U>` becomes `Promise<U>`, not `Promise<Promise<U>>`.
5. **Lock-Free Cross-Thread Resolve** — `PromiseResolver` holds `ArcWeak`; `resolve()` silently drops if Promise is destroyed.
6. **Void-Aware Templates** — `Void` + `FixVoid<T>` maps `void → Void` for uniform generic code.
7. **Nested `.await()` Is Safe** — `WaitScope` owns the loop binding; nested `Run` calls don't unbind it.
8. **Per-Chain Arena** — `.then()` chains bump-allocate nodes in a shared 256-byte arena, reducing malloc calls from O(N) to O(1) per chain. Overflow nodes fall back to heap transparently.
9. **Coroutine-Native** — `Promise<T>` is both a poll-based node container and a C++20 coroutine return type. `co_await` drives the same `poll()` mechanism as `.then()`.
10. **Adapter Pattern** — External async operations (timers, thread pool, custom I/O) bridge into the poll world via `AdapterPromiseNode` + `PromiseResolver`.

## Architecture

```mermaid
graph TD
    subgraph "User API"
        PR["resolve(v)"]
        CHAIN[".then(fn)"]
        AWAIT[".await()"]
        FIBER["xpp::fiber()"]
        ASYNC["async&lt;T&gt;()"]
        PRR["PromiseResolver&lt;T&gt;"]
        CORO["co_await / co_return"]
    end

    subgraph "PromiseNode Hierarchy"
        BASE["PromiseNode&lt;T&gt;<br/>poll(waker) → Option&lt;T&gt;"]
        IMM["ImmediatePromiseNode"]
        TRANS["TransformPromiseNode"]
        CHAINP["ChainPromiseNode"]
        ADAPT["AdapterPromiseNode&lt;T, Adapter&gt;"]
        MANUAL["ManualResolveNode&lt;T&gt;"]
        COROP["CoroutinePromiseNode&lt;T&gt;"]
    end

    subgraph "Shared State"
        RS["ResolveState&lt;T&gt;<br/>Arc / ArcWeak"]
        AW["AtomicPromiseWaker"]
        ARENA["PromiseArena (256B)<br/>per-chain bump allocator"]
        FIB["xFiber / xFiberYield<br/>stackful suspend"]
    end

    PR --> IMM
    CHAIN --> TRANS
    CHAIN --> CHAINP
    ASYNC --> MANUAL
    ASYNC --> PRR
    PRR --> RS
    ADAPT --> RS
    CORO --> COROP
    AWAIT --> BASE
    FIBER --> FIB
    FIB --> AWAIT
    BASE --> AW
    RS --> AW
    TRANS -.->|arena-allocated| ARENA
    CHAINP -.->|arena-allocated| ARENA

    style AWAIT fill:#4a90d9,color:#fff
    style FIBER fill:#50b86c,color:#fff
    style RS fill:#e91e63,color:#fff
    style ADAPT fill:#50b86c,color:#fff
    style ARENA fill:#f5a623,color:#fff
    style CORO fill:#9b59b6,color:#fff
    style FIB fill:#4a90d9,color:#fff
```

## Topics

- [`.await()` & Fiber](await.md) — `.await()` semantics, fiber suspend, event loop integration
- [then()](then.md) — chaining, auto-flatten, type transformations
- [Deferred Resolution](deferred.md) — `async()`, `PromiseResolver`, cross-thread resolve
- [Timers & Timeouts](timers.md) — `after(ms)`, timeout pattern with `race`
- [Combinators](combinators.md) — `all()`, `race()`, concurrent composition
- [Utilities](utils.md) — `try_next()`, sequential fall-through
- [Custom Adapters](adapter.md) — `adapt`, `work`, Adapter contract, `TimerAdapter`, `WorkAdapter`
- [C++20 Coroutines](coroutine.md) — `co_await` / `co_return` with `Promise<T>` (no `Task<T>`)
- [Internals](internals.md) — `PromiseNode` hierarchy, waker system, `ResolveState`

## API Reference

### Promise\<T\>

| Member | Description |
| -------- | ------------- |
| `auto then(Func fn)` | Chain transform. If `fn` returns `Promise<U>`, auto-flattens to `Promise<U>` |
| `Promise<void> discard()` | Drop value, return `Promise<void>` |
| `T await()` | Wait for resolve. Fiber-aware: suspends inside `xpp::fiber()`, blocks otherwise |
| `operator co_await()` | (C++20 only) Await in coroutine. Rvalue-qualified |
| `operator bool()` | True if non-empty (holds a node) |

### PromiseResolver\<T\>

| Member | Description |
| -------- | ------------- |
| `void resolve(T v)` | Fulfill with value. Thread-safe. Silently drops if Promise destroyed |
| `void resolve()` | (void specialization) Fulfill with no value |
| `bool is_pending()` | True if Promise alive and unresolved |

### Free Functions

| Function | Description |
| ---------- | ------------- |
| `resolve(v)` | Immediately-resolved promise. T deduced from argument |
| `yield()` | Immediately-resolved `Promise<void>` |
| `fiber(fn)` | Run `fn` in a stackful fiber (64KB stack). Returns `Promise<decltype(fn())>` |
| `after(ms)` | Resolve after `ms` milliseconds. Returns `Promise<void>` |
| `lazy(fn)` | Wrap sync function as lazy promise; runs on first poll. T deduced from return type |
| `work(fn)` | Run func on thread pool. T deduced from return type |
| `adapt<T, Adapter>(args...)` | Custom adapter-backed promise |
| `async<T>()` | → `pair<Promise<T>, PromiseResolver<T>>` |
| `all(Promise<Ts>...)` | Wait for all → `tuple` or `void` |
| `race(Promise<T>, Promise<T>...)` | First resolved wins |
| `try_next(items, fn)` | Try each item sequentially, return first `ok` |
