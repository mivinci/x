# wait() — Driving the Event Loop

[← Promise\<T\>](README.md)

`wait()` blocks the calling thread and drives the event loop until the promise resolves. It's the bridge between the async world and synchronous code — main, tests, or any place where you need a concrete value.

## Setup: EventLoop & WaitScope

Every `wait()` call needs an `EventLoop` and a `WaitScope` in scope on the calling thread. The `WaitScope` binds the event loop to the thread — nested `wait()` calls are safe because the scope stays alive throughout.

```cpp
xpp::EventLoop loop;       // creates the event loop (epoll / kqueue)
xpp::WaitScope scope(loop); // binds it to this thread

int result = fetch_value()              // Promise<int>
    .then([](int x) { return x * 2; })
    .wait();                            // blocks until resolved
```

If you forget the `WaitScope`, `wait()` panics — the promise needs an event loop to run timers, I/O, and cross-thread callbacks.

## How wait() works

Under the hood, `wait()` does three things in a loop:

1. **Poll** the promise — if it returns `Some<T>`, we're done.
2. **Run** the event loop for one iteration — drain the done queue, fire timers, poll I/O.
3. **Repeat** until the promise resolves or the event loop is stopped.

This is exactly the same model as Tokio's `current_thread` runtime — there's no background thread pool, no work-stealing scheduler. One thread, one event loop.

```cpp
// Pseudocode for what wait() does internally
T Promise<T>::wait() {
    PromiseWaker waker = PromiseWaker::sync_wait(&done);
    while (true) {
        Option<T> result = m_node->poll(waker);  // try to resolve
        if (result.is_some()) return result.unwrap();
        xEventLoopRun(m_loop, X_RUN_ONCE);       // process events
    }
}
```

## Nested wait() is safe

A promise chain can call `wait()` on another promise — the `WaitScope` uses a thread-local pointer, so nested calls don't unbind the outer scope.

```cpp
Promise<User> fetch_user() {
    return fetch_id().then([](int id) {
        return fetch_from_db(id).wait(); // nested wait — safe
    });
}

// Even this works — the outer wait() drives both chains
User u = fetch_user().wait();
```

## wait() in tests

Almost every test in libxpp follows this pattern:

```cpp
xpp::Promise<void> my_test_coroutine() {
    auto result = co_await some_async_op();
    EXPECT_EQ(result, 42);
    co_return;
}

TEST(MyTest, Example) {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);
    my_test_coroutine().wait();  // drive the async test to completion
}
```

## wait() vs co_await

| | `wait()` | `co_await` |
| --- | --- | --- |
| Available in | C++11 | C++20 |
| Blocks thread | Yes | No (suspends coroutine) |
| Use case | main, tests, sync code | Inside another coroutine |
| Underlying mechanism | Polls + runs loop | Polls + registers waker |

They're the same mechanism at different levels — `wait()` runs the loop around `poll()`, while `co_await` hooks into the existing loop's waker system.
