# C++20 Coroutines

[← Promise](README.md)

## When to Use

You have multi-step async flows and want linear code instead of `.then()` chains.

```cpp
// .then() chain — nested callbacks
Promise<int> compute() {
    return Promise<int>::resolve(1)
        .then([](int x) { return x + 1; })
        .then([](int x) { return Promise<int>::work([x] { return x * 2; }); })
        .then([](int x) { return x - 3; });
}

// Coroutine — linear code
Promise<int> compute() {
    int x = co_await Promise<int>::resolve(1);
    x = x + 1;
    x = co_await Promise<int>::work([x] { return x * 2; });
    co_return x - 3;
}
```

## Requirements

C++20 compiler with coroutine support. Guarded by `XPP_HAS_COROUTINES` (defined in `<xpp/compiler.h>`). C++17 code is unaffected.

## Promise\<T\> as Coroutine Return Type

Any function returning `Promise<T>` can be a coroutine. Use `co_return` to produce the result:

```cpp
Promise<int> fetch_value() {
    co_return 42;
}

Promise<void> do_something() {
    co_return;  // void coroutine
}
```

The coroutine is **lazy** — it doesn't start until `wait()` (or `.then()`) drives it via `poll()`.

## co_await Promise\<U\>

`co_await` works on any `Promise<U>` (rvalue). The coroutine suspends until the promise resolves, then the `co_await` expression yields the value:

```cpp
Promise<int> fetch_and_parse() {
    auto data = co_await Promise<std::string>::work(fetch_url);
    auto result = co_await Promise<int>::work([&] { return parse(data); });
    co_return result;
}
```

You can `co_await` any Promise source:
- `Promise::resolve(v)` — immediate
- `Promise<void>::after(ms)` — timer
- `Promise<T>::work(fn)` — thread pool
- `async<T>()` — deferred (pass resolver to another thread)
- `all(...)` / `race(...)` — combinators
- Another coroutine's return value

## co_await void

`co_await Promise<void>` suspends and resumes with no value:

```cpp
Promise<int> delayed_compute() {
    co_await Promise<void>::after(100);  // wait 100ms
    co_return 42;
}
```

## Nested Coroutines

Coroutines can `co_await` other coroutines:

```cpp
Promise<int> inner() {
    co_await Promise<void>::after(10);
    co_return 100;
}

Promise<int> outer() {
    int x = co_await inner();
    co_return x + 1;
}

// outer().wait() == 101
```

## co_await Combinators

```cpp
Promise<int> fetch_both() {
    auto [status, body] = co_await xpp::all(
        Promise<int>::work(fetch_status),
        Promise<std::string>::work(fetch_body)
    );
    co_return status + static_cast<int>(body.size());
}

Promise<int> fetch_with_timeout() {
    int result = co_await xpp::race(
        Promise<int>::work(fetch),
        Promise<void>::after(5000).then([] { return -1; })
    );
    co_return result;
}
```

## Coroutine + then Chain

Coroutines produce regular `Promise<T>`, so they compose with `.then()`:

```cpp
Promise<int> compute() { co_return 10; }

int result = compute().then([](int x) { return x * 3; }).wait();
// result == 30
```

## Early Destruction

If a coroutine's `Promise<T>` is destroyed before completion (e.g., a losing branch in `race()`), the coroutine frame is safely destroyed. The `CoroutinePromiseNode` destructor calls `handle.destroy()`, and any awaited promise's node is released.

```cpp
{
    auto p = slow_coro();  // coroutine that takes 10s
    // p destroyed here — coroutine frame destroyed, no crash
}
```

## How It Works

```
┌──────────────────────────────────────────────────────────────┐
│  CoroutinePromiseNode<T> : public PromiseNode<T>             │
│                                                              │
│  poll(waker):                                                │
│    while (true):                                             │
│      if done → return Some(result)                          │
│      if has await_state:                                    │
│        poll(awaited_promise, waker)                         │
│        not ready → return None                              │
│        ready → clear await_state, resume coroutine          │
│      else:                                                   │
│        resume coroutine (start or continue)                 │
│        (coroutine may co_return or co_await again)          │
│                                                              │
│  coroutine frame:                                            │
│    co_await promise                                         │
│    → PromiseAwaiter::await_suspend(handle)                  │
│    → extract node, store in CoroutinePromiseNode            │
│    → suspend (return to poll)                               │
│    → poll() polls the node on next call                     │
│    → when ready, resume coroutine, await_resume() → value   │
└──────────────────────────────────────────────────────────────┘
```

### Key: while-loop in poll()

After `handle.resume()`, the coroutine may have `co_await`ed an already-resolved promise (e.g., `Promise::resolve`). The `while` loop immediately polls it and resumes — no `None` returned, no busy-loop in `wait()`.

### Type-erased await

A `Promise<int>` coroutine can `co_await Promise<string>`. The awaited node is stored via type-erased `AwaitState`:

- `AwaitStateImpl<U>` — polls `PromiseNode<U>`, stores result in `Option<U>*`
- `VoidAwaitState` — polls `PromiseNode<void>`, sets `bool*` (because `PromiseNode<void>` ≠ `PromiseNode<Void>`)

### std::coroutine_traits

```cpp
namespace std {
template <class T>
struct coroutine_traits<xpp::Promise<T>> {
    using promise_type = xpp::_::CoroutinePromise<T>;
};
}
```

No `Task<T>` wrapper — `Promise<T>` IS the coroutine return type.
