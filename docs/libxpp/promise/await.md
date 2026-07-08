# `.await()` — Waiting for a Promise

[← Promise\<T\>](README.md)

`.await()` is the canonical way to extract a value from a `Promise<T>`. It's context-aware — the same call behaves differently depending on where you are.

```
Outside a fiber:          Inside xpp::fiber():
  park()                    park()
    X_RUN_ONCE               xFiberYield()
    (thread blocks)          (fiber suspends, event loop keeps running)
```

## Two contexts, one API

### 1. Direct (non-fiber) — drives the event loop

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

int result = fetch_value()              // Promise<int>
    .then([](int x) { return x * 2; })
    .await();                           // runs X_RUN_ONCE until resolved
```

### 2. Inside a fiber — non-blocking suspend

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

xpp::fiber([]() {
    auto a = http_get("/a").await();    // fiber suspends
    auto b = http_get("/b").await();    // resumes when a is ready
    return a + b;
}).then([](int total) {
    printf("total = %d\n", total);
});

loop.run();  // one thread drives all fibers + I/O
```

This is the key insight: `.await()` means *"wait for this Promise, letting the event loop continue in the meantime"*. Outside a fiber, **you** run the loop. Inside a fiber, you **return control** to the loop.

## How it works

```cpp
// Simplified — the real implementation is in PromiseWaker::park()
T Promise<T>::await() {
    PromiseWaker waker;       // auto-detects fiber context
    while (true) {
        Option<T> result = m_node->poll(waker);
        if (result.is_some()) return result.unwrap();
        waker.park();         // fiber: xFiberYield | non-fiber: X_RUN_ONCE
    }
}
```

1. **Poll** — ask the promise node if the value is ready.
2. **Park** — if not ready, let the event loop make progress:
   - **Fiber**: suspend via `xFiberYield()`, the event loop runs on the main stack, and the waker switches back when the promise resolves.
   - **Non-fiber**: call `xEventLoopRun(X_RUN_ONCE)` to process one batch of events (timers, I/O, done queue).
3. **Repeat** until `poll()` returns `Some<T>`.

## Nested .await() is safe

A promise chain can call `.await()` on another promise — the `WaitScope` uses a thread-local pointer, so nested calls don't unbind the outer scope.

```cpp
Promise<User> fetch_user() {
    return fetch_id().then([](int id) {
        return fetch_from_db(id).await(); // nested await — safe
    });
}

User u = fetch_user().await();
```

## .await() in tests

Almost every test follows this pattern:

```cpp
TEST(MyTest, Example) {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);
    my_test_coroutine().await();  // drive to completion
}
```

## `await()` vs `co_await`

| | `.await()` | `co_await` |
| --- | --- | --- |
| Available in | C++11 + fiber | C++20 |
| Blocks thread | Yes (if not in fiber) | No (suspends coroutine) |
| Inside fiber | Suspends fiber (non-blocking) | N/A |
| Use case | main, tests, fibers, sync code | Inside another coroutine |
| Mechanism | `poll()` + `park()` | `poll()` + compiler-generated state machine |

They're the same mechanism at different levels. `.await()` is the universal entry point — it works everywhere. `co_await` is a C++20 sugar inside coroutine functions.
