# Utilities: try\_next

[← Promise](README.md)

## When to Use

You need to try multiple items with an async function, returning the first success — like falling through a list of DNS addresses until one connects.

## `xpp::try_next(items, fn)`

Calls `fn(item)` on each item in `items` sequentially. Returns the first `ok` result, or the last `err` if all fail.

- **C++11-compatible**: uses `struct + std::move(*this)` chaining, no coroutines or concepts
- **Zero heap allocation** in the combinator itself (items and fn stored by value)
- **Duck-typed**: `fn(item)` must return `Promise<Result<T, E>>` where `Result` has `.is_ok()`
- **Factory function**: `try_next(items, fn)` returns a callable; invoke with `()` to start

```cpp
#include <xpp/promise_utils.h>

// Try each resolved DNS address until one connects
std::vector<SocketAddr> addrs = co_await resolve_host("example.com");
auto stream = xpp::try_next(std::move(addrs), [&](const SocketAddr &a) {
    return TcpStream::connect_with_conf(a.ip().c_str(), a.port(), conf.get());
})().await();

// Returns first ok, or last error (ConnectionRefused from final address)
```

### Immediate results

When `fn` returns immediately-resolved promises, `try_next` evaluates without touching the event loop:

```cpp
std::vector<int> items = {10, 20, 30};

auto result = xpp::try_next(std::move(items), [](int x) -> Promise<Result<int, int>> {
    if (x == 20) return xpp::resolve(Result<int, int>(ok, x * 10));
    return xpp::resolve(Result<int, int>(err, -x));
})().await();
// result == 200 (20 × 10), only tried 10 (failed) and 20 (ok)
```

### Fall-through chain

When all items fail, returns the last error:

```cpp
std::vector<int> items = {1, 2, 3};

auto err = xpp::try_next(std::move(items), [](int x) -> Promise<Result<int, int>> {
    return xpp::resolve(Result<int, int>(err, x));
})().await();
// err == 3, all three were tried
```

### Deferred (async) results

Works with async operations scheduled on the event loop:

```cpp
std::vector<int> items = {10, 20, 30};

auto ar1 = xpp::async<Result<int, int>>();
auto ar2 = xpp::async<Result<int, int>>();

// r1 resolves with error at 10ms, r2 resolves with success at 20ms
schedule_resolve(ar1.second, Result<int, int>(err, -1), 10);
schedule_resolve(ar2.second, Result<int, int>(ok, 99), 20);

auto result = xpp::try_next(std::move(items), [&, p1 = std::move(ar1.first),
                                                  p2 = std::move(ar2.first)]
                            (int) mutable -> Promise<Result<int, int>> {
    static int call_count = 0;
    call_count++;
    if (call_count == 1) return std::move(p1);
    return std::move(p2);
})().await();
// result == 99 (second item succeeded after the first failed)
```

## How It Works

`TryNext<Items, Func>` is a callable struct. Calling `operator()` pushes the first item through `fn()`, then chains a `.then()` callback:

```cpp
// Simplified:
struct TryNext {
    Items items;
    size_t idx;
    Func fn;

    P operator()() {
        return fn(items[idx++]).then(Then{std::move(*this)});
    }

    struct Then {
        TryNext next;  // ownership transferred via move
        template <class R>
        P operator()(R &&r) {
            if (r.is_ok()) return xpp::resolve(std::forward<R>(r));
            if (next.idx >= next.items.size())
                return xpp::resolve(std::forward<R>(r));  // last error
            return next();  // try next item
        }
    };
};
```

Key design points:
- **`std::move(*this)` ownership transfer**: the `TryNext` struct (with `items` and `fn` by value) moves through each `.then()` node in the Promise chain — no `shared_ptr` refcount overhead
- **Template `operator()` in `Then`**: accepts `Result<T, E>` without spelling out the types, enabling duck-typing of any `Result` type with `.is_ok()`
- **Tail-recursive via Promise chain**: `return next()` creates a new `.then()` link, rather than growing the call stack

## Performance

| Allocation | Count |
| ------------ | ------- |
| TryNext struct (items + fn) | 0 (stack/inline) |
| PromiseNode chain | 1 heap (head) + N arena bumps → 1 bulk free |

The combinator itself is zero-allocation. The Promise chain uses xpp's per-chain arena allocator, so only the head node hits the heap — subsequent `.then()` nodes are bump-allocated in the arena.

## Why Not `try_each`?

The original name was `try_each`, but that misleadingly implies all items are **always** tried. The actual semantics are "try one, fail → try the next, succeed → stop". `try_next` captures this accurately: it parallels `try_next()` / `next()` iteration patterns in Rust's `Iterator` trait.
