## Why

libxpp currently has no general-purpose callback-style timer. The only
timer accessible from C++ is `Promise<void>::after(ms)`, which is one-shot
and Promise-based. For periodic / repeating tasks — heartbeats, polling,
tickers, retries — users must either:

1. Drop down to the raw C API (`xTimerStart` + `xTimerStop` + manual
   `on_cancel` plumbing), losing RAII and type-safety.
2. Chain `Promise::after` recursively, which is awkward and allocates
   a new `TimerPromiseNode` per tick.

Neither is ergonomic. This change adds `xpp::Timer`, a move-only RAII
wrapper around `xTimerStart` / `xTimerStop` that supports both one-shot
and repeating timers via a callback API, with pause/resume (`stop()`
/ `start()`) and safe shutdown via the `on_cancel` hook.

## What Changes

- New class `xpp::Timer` in `libxpp/xpp/timer.h`:
  - Template constructor: `Timer(uint64_t timeout_ms, uint64_t repeat_ms, F&& cb)`
  - Convenience overload: `Timer(uint64_t ms, F&& cb)` (repeating with
    `timeout_ms == repeat_ms == ms`)
  - `stop()` / `start()` for pause/resume (idempotent; `start()` uses
    original timeout/repeat)
  - `is_active()`, `operator bool`, `handle()` observers
  - RAII: destructor calls `stop()` if active
  - Move-only (movable via `Own<State>` indirection)
- `Timer` is **not** a `PromiseNode`. It is a callback-style primitive
  that complements `Promise<void>::after(ms)`. The two coexist:
  - `after(ms)` → one-shot, Promise-based, integrates with `.then()`
  - `Timer(timeout, repeat, cb)` → callback-based, supports repeating
- `Timer` uses the `on_cancel` hook (added in the `x-timer-on-cancel`
  change, PR #7) for safe shutdown when the host loop is destroyed.
- New test file `libxpp/xpp/timer_test.cpp` covering: one-shot,
  repeating, pause/resume, callback self-stop, move, loop-destroy
  cleanup, and the one-shot `fire_cb` handle-nulling invariant.
- New doc page `docs/libxpp/timer.md` describing the API, semantics,
  and usage examples.

## Capabilities

### New Capabilities

- `xpp-timer`: callback-style timer with pause/resume, supporting both
  one-shot and repeating modes, with safe shutdown via `on_cancel`.

### Modified Capabilities

_None — this is a new class, not a modification to existing behavior._

## Impact

- **libxpp/xpp/timer.h**: new header (~120 lines), no dependencies
  beyond `<xpp/own.h>`, `<xpp/event.h>` (transitively pulls libx), and
  `<atomic>` (for the State struct).
- **libxpp/xpp/timer_test.cpp**: new test file (~200 lines).
- **libxpp/xpp/CMakeLists.txt**: add `timer.h` to headers, `timer_test.cpp`
  to test sources, register `timer_test` with CTest.
- **docs/libxpp/timer.md**: new doc page.
- **docs/SUMMARY.md**: add link to timer page.
- **Dependencies**: requires `xTimerStart`'s `on_cancel` parameter
  (merged in PR #7). No new external dependencies.
- **ABI**: none — libxpp is header-only.
