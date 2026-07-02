# timer.h — Callback-Style Timer

## Introduction

`timer.h` provides `xpp::Timer`, a move-only RAII wrapper around
`xTimerStart` / `xTimerStop`. It supports both **one-shot** and
**repeating** timers via a callback API, with `stop()` / `start()` for
pause/resume.

`Timer` complements `Promise<void>::after(ms)`:

| | `Promise<void>::after(ms)` | `xpp::Timer` |
|---|---|---|
| Style | Promise-based (poll/wait) | Callback-based |
| Modes | One-shot only | One-shot + repeating |
| Composition | `.then()`, `.wait()` | None (just fires callback) |
| Use case | Delayed computation in a Promise chain | Periodic tasks, heartbeats, simple delayed callbacks |

## API Reference

### Construction

| Expression | Behavior |
|---|---|
| `Timer(ms, cb)` | Repeating: fires every `ms` (timeout = repeat = ms) |
| `Timer(timeout, repeat, cb)` | Explicit: first fire after `timeout`, then every `repeat`. `repeat == 0` = one-shot |

`cb` is a callable with signature `void()`. It is stored by value
(decay-copy) inside a heap-allocated `State`.

### Methods

| Method | Returns | Description |
|---|---|---|
| `stop()` | `void` | Cancel the timer. Idempotent. |
| `start()` | `bool` | Resume after `stop()`. `false` if already active or no live loop. |
| `is_active()` | `bool` | True if timer is currently scheduled. |
| `operator bool()` | `bool` | Equivalent to `is_active()`. |
| `handle()` | `xTimer` | Underlying handle for C interop, or `nullptr`. |

### Lifetime

- **Move-only**: copy is deleted. Move transfers ownership of the
  internal `State`.
- **RAII**: destructor calls `stop()` if the timer is active.
- **WaitScope contract**: must be constructed within a `WaitScope`.
  The callback runs on the `WaitScope` thread.

## Usage Examples

### Repeating timer

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

int ticks = 0;
xpp::Timer t(100, [&]() {
  if (++ticks >= 5) loop.stop();
});

loop.run();
// 5 ticks, ~500ms
```

### One-shot delayed callback

```cpp
xpp::Timer t(1000, 0, [&]() {
  printf("1 second elapsed\n");
});
```

### Asymmetric repeating (fast first fire, slow subsequent)

```cpp
// First fire at 10ms, then every 5s
xpp::Timer t(10, 5000, [&]() { poll_device(); });
```

### Pause and resume

```cpp
xpp::Timer t(100, [&]() { heartbeat(); });

// ... later, pause ...
t.stop();

// ... even later, resume ...
t.start();  // next fire is 100ms from now (not from when we paused)
```

### Self-stop from callback

```cpp
int n = 0;
xpp::Timer *t_ptr = nullptr;
xpp::Timer t(100, [&]() {
  if (++n >= 3) t_ptr->stop();
});
t_ptr = &t;
```

Calling `stop()` from inside the callback is safe — libx re-arms
repeating timers before invoking the callback, so `xTimerStop` finds
a valid timer in the heap and removes it.

## Notes

### Callback exceptions

If the callback throws, the exception propagates through
`xEventLoopRun` to the caller of `loop.run()`. Throwing callbacks are
the user's responsibility.

### No deadline preservation on resume

`stop()` discards the original deadline. `start()` schedules a fresh
timer with the original `timeout_ms` / `repeat_ms`. The next fire is
`timeout_ms` away, regardless of when `stop()` was called.

This matches libuv's `uv_timer_stop` / `uv_timer_start` semantics.

### Loop-destroy cleanup

When the host event loop is destroyed with the timer still pending,
libx invokes the `on_cancel` hook (added in the `x-timer-on-cancel`
change). The hook nulls the stored handle, so `~Timer` skips
`xTimerStop`. The user callback is NOT invoked on this path.

## Comparison with `Promise<void>::after(ms)`

```cpp
// Promise-based (use for Promise composition):
Promise<void>::after(100).then([]() {
  return compute_result();
}).wait();

// Callback-based (use for periodic tasks or simple callbacks):
xpp::Timer t(100, []() {
  heartbeat();
});
```

Use `after(ms)` when you need `.then()` / `.wait()` composition. Use
`Timer` when you need a periodic callback or a simple one-shot callback
without Promise overhead.
