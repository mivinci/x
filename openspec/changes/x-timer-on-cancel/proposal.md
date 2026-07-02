## Why

`xTimerStart` lets callers attach a `void *arg` to a timer, but libx provides
no hook to clean up that arg when the timer is destroyed without firing
(notably during `xEventLoopDestroy`). This leaks user state in two cases:
(1) the event loop is torn down while timers are pending, and (2) any
wrapper that delegates arg ownership to the timer cannot gracefully
release the arg on shutdown.

`xWorkSubmit` already solves this with its `on_cancel` parameter — the
same pattern is needed for `xTimer`.

## What Changes

- **BREAKING**: `xTimerStart` gains an `on_cancel` parameter between
  `arg` and `timeout_ms`:
  ```c
  xTimer xTimerStart(xTimerFunc fn, void *arg,
                     xTimerFunc on_cancel,
                     uint64_t timeout_ms, uint64_t repeat_ms);
  ```
- `xTimer_` struct gains an `on_cancel` field (set at start, invoked
  only on the loop-destroy path).
- Backend destroy paths (`kq_destroy`, `epoll_destroy`, `poll_destroy`,
  `wsapoll_destroy`) call `t->on_cancel(t->arg)` before `timer_free`
  for every pending timer. `on_cancel == NULL` is a no-op.
- `xTimerStop` **does not** invoke `on_cancel` — stop returns arg
  ownership to the caller (semantic A: stop is user-initiated, not
  cancellation).
- Timer fire path (`loop_run_timers`) **does not** invoke `on_cancel`
  — `fn(arg)` is the only callback on the fire path. `fn` and
  `on_cancel` are mutually exclusive; exactly one runs per timer
  lifetime.
- All in-tree callers of `xTimerStart` updated to pass `NULL` (or a
  real cleanup callback where appropriate).
- All timer tests updated to the new signature; new tests cover the
  destroy-path invocation.

## Capabilities

### New Capabilities

_None — this change extends an existing capability, it does not
introduce a new one._

### Modified Capabilities

- `event-loop-timer`: add a new requirement that timers support an
  optional `on_cancel` cleanup callback invoked when the host event
  loop is destroyed with the timer still pending.

## Impact

- **libx/x/base/event.h**: signature change to `xTimerStart`; new
  `on_cancel` semantics documented in the API contract.
- **libx/x/base/event_private.h**: `struct xTimer_` gains an
  `on_cancel` field; `timer_alloc` zero-inits it (already memset to
  0, so absent callers behave as before).
- **libx/x/base/event_timer.c**: `submit_timer` stores `on_cancel`;
  `xTimerStop` semantics unchanged.
- **libx/x/base/event_kqueue.c**, **event_epoll.c**, **event_poll.c**,
  **event_wsapoll.c**: `*_destroy` paths invoke `on_cancel` before
  `timer_free` for each pending timer.
- **libx/x/base/event_timer_*_test.cpp** and any other call sites
  under `libx/`: updated to the new signature.
- **libdlproxy**, **libxpp** (Promise::after): all `xTimerStart`
  call sites updated.
- **ABI**: This is a source-breaking signature change. There is no
  shared-library version bump because libx builds as a static archive
  by default; consumers recompile.
- **CI**: Must pass on Linux + macOS, openssl + mbedtls (the existing
  4-config matrix).
