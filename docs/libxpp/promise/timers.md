# Timers & Timeouts

[← Promise](README.md)

## When to Use

You need a delay, a timeout, or want to race a Promise against a timer.

## `Promise<void>::after(ms)`

Resolves after `ms` milliseconds. Available only on `Promise<void>`. Chain with `.then()` to run code after the delay.

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

xpp::Promise<void>::after(100)
    .then([]() { printf("100ms elapsed\n"); })
    .wait();
```

Internally uses `AdapterPromiseNode<void, TimerAdapter>`. The `TimerAdapter` owns an `xTimer` handle:

- **Timer fires** → callback calls `m_resolver.resolve()` → sets `resolved=true`, wakes poller
- **Promise destroyed early** → `~TimerAdapter()` calls `xTimerStop` (if not yet fired, checked via `m_fired` atomic flag)
- **Loop destroyed** → `on_cancel` callback nulls `m_handle`, sets `m_fired=true`

The promise must be destroyed on the same WaitScope thread.

## Timeout Pattern with `race`

Combine `after()` with `race()` to implement timeouts:

```cpp
#include <xpp/promise_combinators.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

// Fetch takes 100ms, timeout is 10ms → timeout wins
int result = xpp::race(
    xpp::Promise<void>::after(100).then([] { return 200; }),  // "fetch"
    xpp::Promise<void>::after(10).then([] { return -1; })     // timeout
).wait();
// result == -1 (timeout)
```

When `race` resolves, the losing branch is destroyed. `TimerAdapter`'s destructor calls `xTimerStop` — the 100ms timer is cancelled, no callback fires after destruction.

## Sequential Delays

```cpp
xpp::Promise<void>::after(10)
    .then([]() { return xpp::Promise<void>::after(20); })  // auto-flattened
    .then([]() { printf("30ms total\n"); })
    .wait();
```

## Void Promise Chains

```cpp
int counter = 0;
xpp::Promise<void>::resolve()
    .then([&]() { counter++; })
    .then([&]() { counter++; })
    .wait();
// counter == 2
```

## `defer()` and `yield()`

```cpp
// defer: wrap a sync function as a promise
int result = xpp::Promise<void>::defer([] { return 42; }).wait();
// result == 42

// yield: immediately-resolved Promise<void>, chain entry point
int val = xpp::yield().then([] { return 1; }).wait();
// val == 1
```
