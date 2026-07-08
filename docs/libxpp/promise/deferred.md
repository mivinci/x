# Deferred Resolution

[← Promise](README.md)

## When to Use

You have an async operation that completes later (timer, network, thread pool) and want to create a Promise that resolves when it finishes.

## `xpp::async<T>()`

Returns `std::pair<Promise<T>, PromiseResolver<T>>`. The resolver is safe to call after the Promise is destroyed — it holds `ArcWeak`, so `resolve()` silently drops if the Promise is gone.

```cpp
#include <xpp/promise.h>
#include <xpp/promise_adapter.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [p, r] = xpp::async<int>();

// Resolve from a timer callback
xpp::Timer t(100, 0, [&]() { r.resolve(42); });

int result = p.await();  // blocks ~100ms
// result == 42
```

## Cross-Thread Resolve

`resolve()` is thread-safe — `ArcWeak::upgrade()` is a CAS loop. Safe to call from any thread.

```cpp
#include <thread>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [p, r] = xpp::async<std::string>();

std::thread worker([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.resolve(std::string("from another thread"));
});

std::string result = p.await();
// result == "from another thread"
worker.join();
```

## Safe After Promise Destruction

If the Promise is destroyed before `resolve()` is called (e.g., a losing branch in `race()`), `resolve()` silently drops — no UAF.

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

xpp::PromiseResolver<int> r;
{
    auto [p, r2] = xpp::async<int>();
    r = std::move(r2);
    // p is destroyed when the scope ends
}
// Promise is gone. resolve() safely drops.
r.resolve(42);
// No crash.
```

## Double Resolve

Only the first `resolve()` takes effect. Subsequent calls are silently dropped via `compare_exchange_strong` on the `resolved` flag.

```cpp
auto [p, r] = xpp::async<int>();
r.resolve(42);
r.resolve(99);  // silently dropped
EXPECT_EQ(p.await(), 42);
```

## Nested wait()

`wait()` inside a `.then()` callback is safe — `WaitScope` owns the loop binding, nested `xEventLoopRun` doesn't unbind it.

```cpp
auto [outer_p, outer_r] = xpp::async<int>();
auto [inner_p, inner_r] = xpp::async<int>();

// Schedule: outer at 60ms, inner at 30ms
// ...

int result = outer_p
    .then([&](int outer_val) {
        int inner_val = inner_p.await();  // nested Run
        return outer_val + inner_val;
    })
    .await();
```

## Best Practices

- **`PromiseResolver` can safely outlive the Promise.** `ArcWeak` — `resolve()` silently drops. No UAF.
- **`is_pending()` is not atomic.** Check only from the owner thread.
- **Don't `wait()` on an empty promise.** Check `operator bool()` first.
- **Nested `wait()` is safe but beware deadlocks.** If the inner promise is never resolved, `wait()` spins indefinitely.
