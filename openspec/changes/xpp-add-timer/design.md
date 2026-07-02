## Context

libxpp lacks a callback-style timer. `Promise<void>::after(ms)` covers
one-shot Promise-based delays, but repeating timers and callback-style
one-shots both require dropping to libx's C API. This change adds
`xpp::Timer`, a move-only RAII wrapper around `xTimerStart` /
`xTimerStop`.

Key constraints from the codebase:
- `xTimerStart` already has `timeout_ms` (initial delay) and
  `repeat_ms` (interval, 0 = one-shot) — the wrapper just exposes both.
- `on_cancel` (PR #7) gives a hook for loop-destroy cleanup.
- libx's fire path for one-shot timers calls `timer_free` BEFORE
  invoking `fn(arg)`, so the `xTimer` handle is dangling after a
  one-shot fire. The wrapper must handle this.
- libx's fire path for repeating timers re-arms the timer BEFORE
  invoking `fn(arg)`, so the handle stays valid across fires.

## Goals / Non-Goals

**Goals:**
- Provide a callback-style timer API that supports both one-shot and
  repeating modes via the same class
- Support pause/resume (`stop()` / `start()`) for both modes
- Use `on_cancel` for safe loop-destroy cleanup (first non-internal
  consumer of the hook beyond `TimerPromiseNode`)
- Template constructor to avoid `std::function` heap allocation
- Move-only, RAII, no manual resource management
- Coexist with `Promise<void>::after(ms)` — they serve different idioms

**Non-Goals:**
- Replacing `Promise<void>::after(ms)`. The two coexist; `after` is
  Promise-based, `Timer` is callback-based.
- Async generator / Stream abstraction over timer ticks. If needed,
  compose `Timer` with `PromiseResolver` manually.
- Cancel-with-cause / deadline preservation on pause. `stop()` loses
  the original deadline; `start()` schedules a fresh timer.
- Per-timer priority or scheduling policy. libx has a single timer
  heap; the wrapper doesn't add scheduling.

## Decisions

### D1: Class named `Timer`, not `Ticker` or `IntervalTimer`

**Decision:** Name the class `xpp::Timer`.

**Why:**
- `Timer` is neutral and covers both one-shot and repeating modes.
  `Ticker` implies repeating, which is misleading when `repeat_ms == 0`.
- Matches `xEventLoop` / `xpp::EventLoop` naming pattern: the C++
  wrapper uses the same noun as the C handle, in a different namespace.
- `IntervalTimer` is verbose; `Timer` is concise.
- `xTimer` (C) and `xpp::Timer` (C++) don't conflict — different
  namespaces, and the convention is already established with
  `xEventLoop` / `xpp::EventLoop`.

**Alternatives considered:**
- `Ticker`: rejected — implies repeating, misleading for one-shot.
- `IntervalTimer`: rejected — verbose, and "interval" also implies
  repeating.
- `Timeout`: rejected — implies one-shot, misleading for repeating.

### D2: Two constructor overloads, not one with defaults

**Decision:**
```cpp
template <class F>
Timer(uint64_t ms, F &&cb);                          // (1) repeating

template <class F>
Timer(uint64_t timeout_ms, uint64_t repeat_ms, F &&cb);  // (2) explicit
```

Constructor (1) is sugar for `Timer(ms, ms, cb)` — symmetric repeating
where the first fire happens at `ms` and subsequent fires at `ms`
intervals. This is the common case.

Constructor (2) is the explicit form covering:
- One-shot: `Timer(100, 0, cb)`
- Asymmetric repeating: `Timer(100, 200, cb)` (first fire at 100ms,
  then every 200ms)
- Immediate first fire: `Timer(0, 100, cb)`

**Why not single constructor with defaults?**
```cpp
template <class F>
Timer(uint64_t timeout_ms, uint64_t repeat_ms = 0, F &&cb);
```
This doesn't work — `F` can't follow a defaulted parameter (C++ doesn't
deduce types after defaulted params). The two-overload form is the
cleanest way to provide both ergonomics and full control.

**Why not named factories (`Timer::once(ms, cb)`, `Timer::repeat(ms, cb)`)?**
They obscure the underlying `timeout_ms` / `repeat_ms` distinction and
add API surface without information gain. The two constructors are
unambiguous and match libx's parameter naming.

### D3: Template constructor + virtual State for type erasure

**Decision:** The constructor is templated on the callable type `F`.
Internally, `F` is stored in a heap-allocated `StateImpl<F>` that
derives from a polymorphic `State` base. `State::invoke()` is virtual.

```cpp
struct State {
  uint64_t     timeout_ms;
  uint64_t     repeat_ms;
  xTimer       handle;
  virtual void invoke()  = 0;
  virtual ~State()       = default;  // for safe ~StateImpl<F> dispatch
};
template <class F>
struct StateImpl : State {
  F f;
  void invoke() override { f(); }
};
```

**Why virtual instead of static thunk<F>?**
- `virtual ~State()` is required anyway for correct cleanup of `F`'s
  captured resources — the vtable already exists.
- Adding `virtual invoke()` to the same vtable is one extra slot at
  zero incremental cost.
- The static-thunk alternative (one templated thunk per `F`, no virtual
  `invoke`) saves ~1-2ns per fire from avoiding the indirect call, but
  timer callbacks fire at ms scale — the overhead is unmeasurable.
- Virtual `invoke()` keeps `fire_cb` as a single non-templated function,
  which simplifies the implementation and matches OOP conventions.

**Alternatives considered:**
- `std::function<void()>`: rejected — adds its own heap allocation
  (SBO + type erasure + copy semantics) on top of our `State` heap
  allocation. Two allocations per timer is wasteful when one suffices.
- Static thunk<F> (TimerPromiseNode's pattern): rejected for `Timer`
  because `TimerPromiseNode` didn't store a user callable (its
  "callback" was hardcoded `resolve()`). `Timer` does store one, so the
  type-erasure overhead is unavoidable; virtual is the simpler choice.

### D4: `Own<State>` indirection for stable libx `arg` pointer

**Decision:** `Timer` holds `Own<State> m_state`. The `xTimer`'s `arg`
parameter points to `m_state.get()`, not to `this`.

**Why:** `Timer` is movable. If `xTimer`'s `arg` pointed to `this`,
moving the `Timer` would leave libx with a dangling pointer to the
moved-from object. `Own<State>` is a stable heap allocation — moving
the `Timer` transfers ownership of the `State` but the `State`'s
address doesn't change.

```
Timer move sequence:
  Before:   Timer A owns State* s
            libx timer.arg = s
  Move:     Timer B takes Own<State>(s)
            Timer A.m_state = nullptr
  After:    Timer B owns State* s (same address)
            libx timer.arg = s  (still valid!)
```

**Trade-off:** One extra heap allocation (the `State`) per `Timer`.
This is unavoidable for any type-erased movable callback wrapper.

### D5: `stop()` / `start()` for pause/resume; no deadline preservation

**Decision:**
- `stop()` cancels the current timer via `xTimerStop(handle)`, then
  nulls `m_state->handle`. Idempotent.
- `start()` schedules a new timer via `xTimerStart(...)` with the
  original `timeout_ms` and `repeat_ms` stored in `State`. Returns
  `false` if already active or no live loop on the current thread.
- Neither method preserves the original deadline. After `start()`, the
  next fire is `timeout_ms` away, regardless of how long ago `stop()`
  was called.

**Why not preserve deadline?**
- `xTimerStop` discards the deadline (the timer struct is recycled).
- To preserve, we'd need to compute `remaining = deadline - now` and
  pass that as the new `timeout_ms` on `start()`. This requires reading
  the timer's internal `deadline` field, which is not exposed by the
  public API.
- The "fresh interval on resume" semantic matches libuv's
  `uv_timer_stop` / `uv_timer_start` and is what most users expect.
- Users who need deadline preservation can compute it themselves and
  construct a new `Timer` with the remaining time.

**Alternatives considered:**
- `pause()` / `resume()` names: rejected — implies deadline
  preservation which we don't provide.
- Separate `pause()` (preserves) vs `stop()` (doesn't): rejected —
  too much API surface for a niche use case.

### D6: `fire_cb` nulls `handle` for one-shot; leaves it for repeating

**Decision:** `fire_cb` checks `m_state->repeat_ms` after invoking the
user callback:
- `repeat_ms == 0` (one-shot): `m_state->handle = nullptr` — libx has
  already called `timer_free` on the timer struct, so the handle is
  dangling. Nulling it prevents `~Timer` or `stop()` from calling
  `xTimerStop` on a dangling handle.
- `repeat_ms > 0` (repeating): leave `handle` alone — libx has
  re-armed the timer, so the handle is still valid.

**Why this works:**
```
libx loop_run_timers (event_private.c:84-100):
  if (t->repeat_ms > 0):
    re-arm (push back to heap)    ← handle valid
  else:
    timer_free(t)                 ← handle dangling
  fn(arg)                          ← fire_cb runs here
```

At the moment `fire_cb` runs, the re-arm-or-free decision has already
been made. `fire_cb` can check `repeat_ms` to know which path libx
took, without reading the (potentially dangling) handle.

**Why not always null the handle?**
For repeating timers, the handle is still valid after fire. Nulling it
would make `stop()` and `~Timer` unable to cancel the timer — it would
keep firing until the loop is destroyed.

### D7: `on_cancel_cb` just nulls the handle

**Decision:** `on_cancel_cb(arg)` sets `static_cast<State*>(arg)->handle = nullptr`.

**Why:** `on_cancel` runs during `xEventLoopDestroy`, after libx has
already removed the timer from the heap and called `timer_free`. The
handle is dangling. Nulling it lets `~Timer` and `stop()` detect
"already reclaimed" and skip `xTimerStop`.

`on_cancel_cb` does NOT invoke the user callback — the user's `F` is
not informed of loop destroy. This matches `TimerPromiseNode`'s
contract: `on_cancel` is a cleanup hook, not a "you've been cancelled"
notification.

**Alternatives considered:**
- Invoke user `cb` on cancel: rejected — surprising semantics. Users
  expecting "exactly one cb per timer lifetime" would be confused.
- Set a "cancelled" flag in `State` for user inspection: rejected —
  adds API surface (`was_cancelled()` query) for a niche use case.

### D8: `Timer` is not a `PromiseNode`

**Decision:** `Timer` is a standalone class, not a `PromiseNode<void>`.

**Why:**
- `PromiseNode` is a one-shot abstraction: `poll()` returns `Some(v)`
  once, then dies. Repeating timers don't fit this model.
- `Timer`'s callback-based API is fundamentally different from
  Promise's poll/wait model.
- Users who want Promise integration can compose: create a
  `PromiseResolver<void>`, call `resolve()` inside the `Timer` callback.
  This is explicit and flexible.

**Alternatives considered:**
- `Timer` as `PromiseNode<void>` that resolves on first fire: rejected
  — that's literally `TimerPromiseNode` from `Promise::after()`.
  Duplicating it as `Timer` would be confusing.

## Risks / Trade-offs

- **[Risk] User callback throws an exception**
  - Mitigation: Let it propagate. libx's `loop_run_timers` will
    propagate to `xEventLoopRun`, which propagates to the user's
    `loop.run()`. This matches libuv's behavior (`uv_timer_cb`
    throwing is UB in C, but in C++ it propagates). Document that
    callback exceptions are the user's responsibility.

- **[Risk] User calls `stop()` from inside the timer callback**
  - Mitigation: This is safe. libx re-arms the timer BEFORE calling
    `fn(arg)`, so `xTimerStop` will find a valid timer in the heap,
    remove it, and call `timer_free`. After `stop()` returns,
    `m_state->handle` is null, so the next `~Timer` or `stop()` is a
    no-op. Documented as a supported use case.

- **[Risk] User moves `Timer` while a fire is pending**
  - Mitigation: `Own<State>` indirection ensures libx's `arg` pointer
    stays valid across moves. The moved-from `Timer` has `m_state ==
    nullptr` and is a safe no-op for all operations.

- **[Risk] `start()` after loop destroy calls `xTimerStart` on a dead
  loop**
  - Mitigation: `xTimerStart` internally calls `xEventLoopCurrent()`,
    which returns `nullptr` if no loop is registered. `xTimerStart`
    then returns `nullptr`. `start()` detects this and returns `false`.
    No crash, no UB.

- **[Trade-off] One heap allocation per `Timer` (the `State`)**
  - Accepted: Unavoidable for type-erased movable callbacks. The
    alternative (`Timer<F>` as a template class) would prevent storing
    `Timer` in containers or returning it from functions by value.

- **[Trade-off] Virtual dispatch on every fire (~1-2ns)**
  - Accepted: Negligible for ms-scale timers. Would matter for
    µs-scale hot paths, which is not what `Timer` is for.
