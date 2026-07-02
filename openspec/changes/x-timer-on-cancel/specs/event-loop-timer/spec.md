## ADDED Requirements

### Requirement: `xTimerStart` accepts an `on_cancel` cleanup callback

`xTimerStart` SHALL accept an `on_cancel` parameter of type `xTimerFunc`
between the `arg` parameter and `timeout_ms`. The full signature SHALL be:

```c
xTimer xTimerStart(xTimerFunc fn, void *arg,
                   xTimerFunc on_cancel,
                   uint64_t timeout_ms, uint64_t repeat_ms);
```

`on_cancel` MAY be NULL, in which case no cleanup callback is invoked
when the timer is destroyed without firing.

#### Scenario: Start with NULL on_cancel
- **WHEN** a caller invokes `xTimerStart(fn, arg, NULL, 100, 0)`
- **THEN** a timer is created with `on_cancel = NULL`
- **AND** the timer behaves identically to the pre-change API

#### Scenario: Start with non-NULL on_cancel
- **WHEN** a caller invokes `xTimerStart(fn, arg, cleanup, 100, 0)`
- **THEN** a timer is created with `on_cancel = cleanup`
- **AND** `cleanup` is registered for invocation on the loop-destroy path

#### Scenario: NULL fn is rejected
- **WHEN** a caller invokes `xTimerStart(NULL, NULL, NULL, 100, 0)`
- **THEN** the call returns NULL
- **AND** no timer is created

### Requirement: `on_cancel` is invoked when the host loop is destroyed

When `xEventLoopDestroy` runs and a timer is still pending in the loop's
timer heap, the destroy path SHALL invoke `t->on_cancel(t->arg)` for
that timer before reclaiming the timer struct via `timer_free`.

If `on_cancel` is NULL, the destroy path SHALL silently skip invocation
(no-op).

#### Scenario: Loop destroyed with pending one-shot timer
- **WHEN** a caller starts a one-shot timer with `on_cancel = cleanup`
- **AND** the timer has not yet fired
- **AND** the host event loop is destroyed via `xEventLoopDestroy`
- **THEN** `cleanup(arg)` is invoked exactly once
- **AND** `fn(arg)` is NOT invoked

#### Scenario: Loop destroyed with pending repeating timer
- **WHEN** a caller starts a repeating timer with `on_cancel = cleanup`
- **AND** the host event loop is destroyed via `xEventLoopDestroy`
- **THEN** `cleanup(arg)` is invoked exactly once
- **AND** `fn(arg)` is NOT invoked for the pending instance

#### Scenario: Loop destroyed with multiple pending timers
- **WHEN** multiple timers are pending in the heap with `on_cancel` set
- **AND** the host event loop is destroyed
- **THEN** `on_cancel` is invoked for every pending timer
- **AND** no `fn` is invoked for any pending timer

#### Scenario: NULL on_cancel during loop destroy
- **WHEN** a pending timer has `on_cancel = NULL`
- **AND** the host event loop is destroyed
- **THEN** no cleanup callback is invoked for that timer
- **AND** no crash or NULL dereference occurs

### Requirement: `xTimerStop` MUST NOT invoke `on_cancel`

`xTimerStop` is user-initiated cancellation. Ownership of `arg` SHALL
return to the caller. `on_cancel` SHALL NOT be invoked by the stop
path.

This diverges intentionally from `xWorkSubmit`'s `on_cancel`, which
fires on `xWorkCancel`. The divergence supports the common timer
"stop, mutate state, restart with the same arg" idiom.

#### Scenario: User-initiated stop does not fire on_cancel
- **WHEN** a caller starts a timer with `on_cancel = cleanup`
- **AND** the caller invokes `xTimerStop(t)` before the timer fires
- **THEN** `cleanup(arg)` is NOT invoked
- **AND** `fn(arg)` is NOT invoked
- **AND** ownership of `arg` returns to the caller
- **AND** `xTimerStop` returns `xErrno_Ok`

#### Scenario: Stop after fire returns InvalidState
- **WHEN** a one-shot timer has already fired
- **AND** the caller invokes `xTimerStop(t)`
- **THEN** `cleanup(arg)` is NOT invoked
- **AND** `fn(arg)` has already been invoked (on the fire path)
- **AND** `xTimerStop` returns `xErrno_InvalidState`

### Requirement: `fn` and `on_cancel` are mutually exclusive

For any single timer lifetime, exactly one of `fn(arg)` or
`on_cancel(arg)` SHALL be invoked, never both, never neither (assuming
the timer struct is reclaimed via either fire or loop destroy).

#### Scenario: Fire path invokes fn, not on_cancel
- **WHEN** a timer with both `fn` and `on_cancel` set is allowed to fire
- **THEN** `fn(arg)` is invoked exactly once
- **AND** `on_cancel(arg)` is NOT invoked

#### Scenario: Destroy path invokes on_cancel, not fn
- **WHEN** a timer with both `fn` and `on_cancel` set is destroyed
  via `xEventLoopDestroy` before firing
- **THEN** `on_cancel(arg)` is invoked exactly once
- **AND** `fn(arg)` is NOT invoked

#### Scenario: Stop path invokes neither
- **WHEN** a timer is stopped via `xTimerStop` before firing
- **THEN** neither `fn(arg)` nor `on_cancel(arg)` is invoked

### Requirement: `on_cancel` runs on the event loop thread

`on_cancel` SHALL be invoked from the event loop thread during
`xEventLoopDestroy`. It SHALL NOT be invoked from any other thread.

#### Scenario: on_cancel called during destroy
- **WHEN** `xEventLoopDestroy` is called on the loop thread
- **AND** a pending timer has `on_cancel` set
- **THEN** `on_cancel(arg)` runs on the same thread that called
  `xEventLoopDestroy`

### Requirement: `on_cancel` MUST NOT touch the host loop

`on_cancel` runs during loop destruction. The loop's internal structures
(heap, freelist, backend fd) are being torn down. Calling any loop API
(`xTimerStart`, `xEventLoopPost`, `xTimerStop`, `xEventLoopRun`, etc.)
from within `on_cancel` results in undefined behavior.

This restriction SHALL be documented in the `event.h` API contract.

#### Scenario: on_cancel that calls loop API is undefined
- **WHEN** an `on_cancel` callback invokes `xTimerStart` or any other
  loop API
- **THEN** the behavior is undefined
- **AND** the API contract MUST document this restriction

### Requirement: All backends share a single destroy helper

The four backend destroy functions (`kq_destroy`, `epoll_destroy`,
`poll_destroy`, `wsapoll_destroy`) SHALL invoke pending timers'
`on_cancel` callbacks through a shared inline helper defined in
`event_private.h`:

```c
static inline void timer_heap_destroy(struct xEventLoop_ *loop) {
    while (xHeapSize(loop->timer_heap) > 0) {
        struct xTimer_ *t = (struct xTimer_ *)xHeapPop(loop->timer_heap);
        if (t->on_cancel) t->on_cancel(t->arg);
        timer_free(loop, t);
    }
}
```

This consolidates the previously duplicated destroy loop across four
backend files.

#### Scenario: All backends invoke on_cancel on destroy
- **WHEN** any of kqueue/epoll/poll/WSAPoll backends destroys its loop
- **AND** pending timers have `on_cancel` set
- **THEN** `on_cancel` is invoked for each pending timer via the shared
  helper

### Requirement: `xTimer_` struct gains an `on_cancel` field

The internal `struct xTimer_` SHALL gain a `xTimerFunc on_cancel` field.
The field SHALL be set by `submit_timer` at timer creation and read
only by the loop-destroy path. `timer_alloc` already `memset`s the
struct to zero, so an unset `on_cancel` is NULL by default.

The field SHALL NOT affect the heap comparator (`timer_cmp`) or any
other timer operation.

#### Scenario: Struct field is set at start
- **WHEN** `xTimerStart(fn, arg, cleanup, 100, 0)` is called
- **THEN** `t->on_cancel == cleanup` after `submit_timer` returns

#### Scenario: Struct field defaults to NULL when not set
- **WHEN** `timer_alloc` returns a struct from the freelist
- **THEN** `t->on_cancel == NULL` (due to memset)
