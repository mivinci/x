## ADDED Requirements

### Requirement: `xpp::Timer` class provides callback-style timer

`xpp::Timer` SHALL be a move-only RAII class that wraps `xTimerStart`
and `xTimerStop`. It invokes a user-provided callback on timer fire,
supporting both one-shot (`repeat_ms == 0`) and repeating
(`repeat_ms > 0`) modes.

The class SHALL be constructible via two overloads:
- `Timer(uint64_t ms, F&& cb)` — symmetric repeating (timeout = repeat = ms)
- `Timer(uint64_t timeout_ms, uint64_t repeat_ms, F&& cb)` — explicit

`F` is a callable with signature `void()` (no arguments, no return).
`F` is stored by value (decay-copy) inside a heap-allocated `State`.

#### Scenario: Construct a repeating timer
- **WHEN** a caller constructs `Timer(100, []{...})` within a WaitScope
- **THEN** `xTimerStart` is called with `timeout_ms=100, repeat_ms=100`
- **AND** the callback is invoked every ~100ms

#### Scenario: Construct a one-shot timer
- **WHEN** a caller constructs `Timer(100, 0, []{...})` within a WaitScope
- **THEN** `xTimerStart` is called with `timeout_ms=100, repeat_ms=0`
- **AND** the callback is invoked exactly once after ~100ms
- **AND** after the callback runs, `is_active()` returns `false`

#### Scenario: Construct an asymmetric repeating timer
- **WHEN** a caller constructs `Timer(100, 200, []{...})`
- **THEN** the first fire happens at ~100ms
- **AND** subsequent fires happen every ~200ms

#### Scenario: Construct outside a WaitScope
- **WHEN** a caller constructs a `Timer` outside any WaitScope
- **THEN** `xTimerStart` returns `nullptr`
- **AND** `is_active()` returns `false`
- **AND** the callback is never invoked

### Requirement: `Timer` is move-only

`Timer` SHALL be movable but not copyable. Move transfers ownership
of the internal `State` to the destination; the moved-from `Timer`
becomes inactive (`is_active() == false`, all operations are no-ops).

The move SHALL NOT invalidate the `xTimer`'s `arg` pointer, because
the `arg` points to the `State` (a stable heap allocation), not to
the `Timer` instance.

#### Scenario: Move-construct a Timer
- **WHEN** a caller move-constructs `Timer b(std::move(a))`
- **AND** `a` was active
- **THEN** `b.is_active()` returns `true`
- **AND** `a.is_active()` returns `false`
- **AND** the timer continues firing and invoking the callback

#### Scenario: Move-assign a Timer
- **WHEN** a caller move-assigns `b = std::move(a)`
- **AND** `b` was previously active
- **THEN** `b`'s previous timer is stopped (via `xTimerStop`)
- **AND** `b` takes ownership of `a`'s timer
- **AND** `a.is_active()` returns `false`

#### Scenario: Operations on a moved-from Timer
- **WHEN** any operation (`stop()`, `start()`, `handle()`, etc.) is
  called on a moved-from `Timer`
- **THEN** the operation is a safe no-op
- **AND** no crash or undefined behavior occurs

### Requirement: `stop()` cancels the timer

`stop()` SHALL call `xTimerStop(m_state->handle)` if the timer is
active, then set `m_state->handle = nullptr`. `stop()` is idempotent:
calling it on an already-stopped or moved-from `Timer` is a no-op.

`stop()` does NOT invoke the user callback.

#### Scenario: Stop a pending timer
- **WHEN** a caller calls `stop()` on an active timer
- **THEN** `xTimerStop` is called on the handle
- **AND** `m_state->handle` becomes `nullptr`
- **AND** the callback is never invoked again

#### Scenario: Stop an already-stopped timer
- **WHEN** a caller calls `stop()` on a timer that was already stopped
- **THEN** the call is a no-op
- **AND** no crash occurs

#### Scenario: Stop from inside the timer callback
- **WHEN** the user callback calls `stop()` on its own `Timer`
- **THEN** `xTimerStop` removes the timer from the heap (libx re-armed
  it before calling the callback)
- **AND** `m_state->handle` is set to `nullptr`
- **AND** the timer does not fire again

### Requirement: `start()` resumes the timer

`start()` SHALL schedule a new timer via `xTimerStart` using the
original `timeout_ms` and `repeat_ms` stored in `State`. It returns
`true` on success, `false` if the timer is already active or no live
event loop is registered on the current thread.

`start()` does NOT preserve the original deadline. After `start()`,
the next fire is `timeout_ms` away.

#### Scenario: Start after stop
- **WHEN** a caller calls `stop()` then `start()` on a repeating timer
- **THEN** a new `xTimerStart` call is made with the original
  `timeout_ms` and `repeat_ms`
- **AND** the timer resumes firing

#### Scenario: Start when already active
- **WHEN** a caller calls `start()` on an active timer
- **THEN** the call returns `false`
- **AND** no new timer is created

#### Scenario: Start with no live loop
- **WHEN** a caller calls `start()` after the event loop has been
  destroyed
- **THEN** `xTimerStart` returns `nullptr`
- **AND** `start()` returns `false`

### Requirement: Destructor calls `stop()` if active

`~Timer()` SHALL call `stop()` if `m_state` is non-null and
`m_state->handle` is non-null. This ensures RAII cleanup of pending
timers.

#### Scenario: Destroy an active timer
- **WHEN** an active `Timer` is destroyed
- **THEN** `xTimerStop` is called on the handle
- **AND** no callback fires after destruction

#### Scenario: Destroy a stopped timer
- **WHEN** a stopped `Timer` is destroyed
- **THEN** no `xTimerStop` call is made (handle is already null)
- **AND** no crash occurs

#### Scenario: Destroy a moved-from timer
- **WHEN** a moved-from `Timer` is destroyed
- **THEN** no operation is performed (m_state is null)

### Requirement: `on_cancel_cb` handles loop-destroy cleanup

When `xEventLoopDestroy` runs and the timer is still pending in the
heap, libx SHALL invoke the `on_cancel` callback. The `on_cancel`
callback SHALL set `m_state->handle = nullptr` to mark the timer as
reclaimed.

`on_cancel_cb` SHALL NOT invoke the user callback.

#### Scenario: Loop destroyed while timer is active
- **WHEN** the host event loop is destroyed while a `Timer` is active
- **THEN** libx invokes `on_cancel_cb`
- **AND** `m_state->handle` is set to `nullptr`
- **AND** the user callback is NOT invoked
- **AND** when `~Timer` later runs, it sees `handle == nullptr` and
  skips `xTimerStop`

#### Scenario: Loop destroyed while timer is stopped
- **WHEN** the host event loop is destroyed after the timer was stopped
- **THEN** `on_cancel_cb` is NOT invoked (the timer was already removed
  from the heap)
- **AND** no crash occurs

### Requirement: `fire_cb` nulls handle for one-shot timers

When libx invokes the fire callback for a one-shot timer
(`repeat_ms == 0`), it has already called `timer_free` on the timer
struct. The `fire_cb` SHALL set `m_state->handle = nullptr` after
invoking the user callback, to prevent `~Timer` or `stop()` from
calling `xTimerStop` on a dangling handle.

For repeating timers (`repeat_ms > 0`), libx has re-armed the timer
before calling `fire_cb`, so the handle is still valid. `fire_cb`
SHALL leave `m_state->handle` unchanged in this case.

#### Scenario: One-shot fire nulls the handle
- **WHEN** a one-shot timer's callback is invoked
- **THEN** after the user callback returns, `m_state->handle` is
  `nullptr`
- **AND** `is_active()` returns `false`
- **AND** subsequent `stop()` or `~Timer` calls are no-ops

#### Scenario: Repeating fire leaves the handle valid
- **WHEN** a repeating timer's callback is invoked
- **THEN** `m_state->handle` remains non-null
- **AND** `is_active()` returns `true`
- **AND** the timer continues to fire on subsequent intervals

### Requirement: Observers

`Timer` SHALL provide:
- `bool is_active() const noexcept` — returns `true` if the timer has
  a non-null handle (i.e., not stopped, not moved-from, not yet fired
  for one-shot)
- `explicit operator bool() const noexcept` — equivalent to
  `is_active()`
- `xTimer handle() const noexcept` — returns the underlying `xTimer`
  for C API interop, or `nullptr` if inactive

#### Scenario: is_active on a newly constructed timer
- **WHEN** a `Timer` is constructed within a WaitScope
- **THEN** `is_active()` returns `true`

#### Scenario: is_active after stop
- **WHEN** `stop()` is called on an active timer
- **THEN** `is_active()` returns `false`

#### Scenario: handle returns the xTimer for interop
- **WHEN** a caller calls `handle()` on an active timer
- **THEN** the return value is the same `xTimer` returned by
  `xTimerStart`
- **AND** the caller can pass it to other libx APIs
