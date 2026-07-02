## ADDED Requirements

### Requirement: `TimerPromiseNode` owns an `xTimer` handle

`TimerPromiseNode` SHALL own the `xTimer` handle returned by
`xTimerStart`. The handle's lifetime is tied to the node's lifetime.
The node's destructor stops the timer if it has not yet fired.

#### Scenario: Constructor starts a timer
- **WHEN** a `TimerPromiseNode` is constructed with a `uint64_t ms`
  delay
- **THEN** it calls `xTimerStart(fire_cb, this, on_cancel_cb, ms, 0)`
- **AND** stores the returned handle in `m_handle`
- **AND** initializes `m_fired` to `false`

#### Scenario: Destructor stops pending timer
- **WHEN** a `TimerPromiseNode` is destroyed
- **AND** `m_fired` is `false` (timer has not fired)
- **THEN** the destructor calls `xTimerStop(m_handle)`
- **AND** `m_handle` becomes dangling (libx recycles the timer struct)

#### Scenario: Destructor skips stop after fire
- **WHEN** a `TimerPromiseNode` is destroyed
- **AND** `m_fired` is `true` (timer has fired)
- **THEN** the destructor does NOT call `xTimerStop`
- **AND** no use-after-free occurs on the dangling `m_handle`

### Requirement: `fire_cb` sets `m_fired` and wakes the waker

The static `fire_cb` callback (invoked by libx when the timer fires)
SHALL:
1. Set `m_fired` to `true` with `memory_order_release`
2. Call `m_waker.wake()` to notify any pending `poll()`
3. NOT delete the node (ownership stays with `Own<>`)

`fire_cb` runs on the event loop thread.

#### Scenario: Timer fires while polling
- **WHEN** a `Promise<void>::after(ms)` is being `wait()`ed
- **AND** the timer fires
- **THEN** `fire_cb` sets `m_fired=true`
- **AND** `m_waker.wake()` is called
- **AND** the next `poll()` returns `Some(Void)`

#### Scenario: Timer fires without a waiter
- **WHEN** the timer fires
- **AND** no `poll()` is currently pending (no registered waker)
- **THEN** `fire_cb` sets `m_fired=true`
- **AND** `m_waker.wake()` is a no-op (no registered waker)
- **AND** the next `poll()` returns `Some(Void)`

### Requirement: `on_cancel_cb` handles loop-destroy cleanup

The static `on_cancel_cb` callback (invoked by libx during
`xEventLoopDestroy` for pending timers) SHALL:
1. Null `m_handle` (libx has recycled the timer struct)
2. Set `m_fired` to `true` with `memory_order_release`
3. Call `m_waker.wake()` to notify any pending `poll()`

This callback is the first in-tree consumer of the `on_cancel`
parameter added in the `x-timer-on-cancel` change.

#### Scenario: Loop destroyed while timer pending
- **WHEN** a `TimerPromiseNode` exists with `m_fired == false`
- **AND** the host event loop is destroyed via `xEventLoopDestroy`
- **THEN** libx invokes `on_cancel_cb(node)`
- **AND** `m_handle` is set to `nullptr`
- **AND** `m_fired` is set to `true`
- **AND** `m_waker.wake()` is called
- **AND** the destructor (when run later) sees `m_fired=true` and skips
  `xTimerStop`

#### Scenario: `on_cancel_cb` does not call loop APIs
- **WHEN** `on_cancel_cb` runs
- **THEN** it only accesses `TimerPromiseNode` member fields
- **AND** it does NOT call `xTimerStart`, `xTimerStop`, `xEventLoopPost`,
  `xEventLoopRun`, or any other libx API

### Requirement: `poll()` returns `Some(Void)` after fire or cancel

`TimerPromiseNode::poll(waker)` SHALL return `Some(Void)` if
`m_fired` is `true` (either fire or `on_cancel` has run). Otherwise
it SHALL register the waker, re-check `m_fired`, and return `None`
(or `Some` if the re-check succeeds).

#### Scenario: poll before fire
- **WHEN** `poll(waker)` is called
- **AND** `m_fired` is `false`
- **THEN** the waker is registered
- **AND** `m_fired` is re-checked (acquire)
- **AND** if still `false`, returns `None`

#### Scenario: poll after fire
- **WHEN** `poll(waker)` is called
- **AND** `m_fired` is `true`
- **THEN** returns `Some(Void)` immediately

#### Scenario: poll after on_cancel
- **WHEN** `poll(waker)` is called
- **AND** `on_cancel_cb` has run (loop was destroyed)
- **THEN** returns `Some(Void)` immediately
- **AND** no `xTimerStop` is called (the handle is null)

### Requirement: `Promise<void>::after(ms)` returns a `TimerPromiseNode`-backed promise

`Promise<void>::after(ms)` SHALL construct a `TimerPromiseNode` and
return a `Promise<void>` owning it. The function SHALL NOT allocate
a separate `PromiseResolver<void>` or `AdapterPromiseNode`.

The user-visible signature and semantics of `after(ms)` are
unchanged.

#### Scenario: After returns a promise that resolves on fire
- **WHEN** `Promise<void>::after(50)` is called within a WaitScope
- **THEN** a `TimerPromiseNode` is constructed
- **AND** a `Promise<void>` owning it is returned
- **AND** `wait()` on the promise blocks for ~50ms then returns

#### Scenario: After no longer uses PromiseResolver
- **WHEN** `Promise<void>::after(ms)` is called
- **THEN** no `PromiseResolver<void>` is heap-allocated
- **AND** no `AdapterPromiseNode` is heap-allocated
- **AND** exactly one heap allocation occurs: the `TimerPromiseNode`

### Requirement: `TimerPromiseNode` must be destroyed on the WaitScope thread

`~TimerPromiseNode` (and thus `~Promise<void>` for a promise returned
by `after()`) SHALL run on the same thread that owns the WaitScope.
This matches the existing `Promise::wait()` contract.

#### Scenario: Promise destroyed on WaitScope thread
- **WHEN** a `Promise<void>::after(ms)` is dropped on the WaitScope
  thread
- **THEN** `~TimerPromiseNode` runs on the same thread
- **AND** if `m_fired` is `false`, `xTimerStop` is called safely

#### Scenario: Promise destroyed on wrong thread is undefined
- **WHEN** a `Promise<void>::after(ms)` is dropped on a thread other
  than the WaitScope thread
- **THEN** behavior is undefined
- **AND** the API contract MUST document this restriction

### Requirement: Use-after-free on early Promise destruction is fixed

When a `Promise<void>` returned by `after(ms)` is destroyed before
the timer fires, no use-after-free SHALL occur. The timer is stopped
in `~TimerPromiseNode` and `fire_cb` is never invoked.

#### Scenario: Promise dropped before fire
- **WHEN** `Promise<void>::after(1000)` is called
- **AND** the returned promise is destroyed immediately (without
  calling `wait()`)
- **AND** the WaitScope thread continues running the event loop
- **THEN** `~TimerPromiseNode` calls `xTimerStop(m_handle)`
- **AND** `fire_cb` is never invoked
- **AND** no use-after-free occurs

### Requirement: Leak on loop-destroy-before-fire is fixed

When the host event loop is destroyed before a `TimerPromiseNode`'s
timer fires, no memory SHALL leak. The `on_cancel_cb` runs during
loop destroy, marks the node as fired, and the node is subsequently
destroyed normally by its owning `Promise`.

#### Scenario: Loop destroyed before fire, Promise still alive
- **WHEN** `Promise<void>::after(1000)` is called
- **AND** the host event loop is destroyed (via `xEventLoopDestroy`)
  before the timer fires
- **AND** the `Promise<void>` is still alive
- **THEN** `on_cancel_cb` runs during `xEventLoopDestroy`
- **AND** `m_fired` is set to `true`
- **AND** `m_handle` is set to `nullptr`
- **AND** when the `Promise` is later destroyed, `~TimerPromiseNode`
  sees `m_fired=true` and does NOT call `xTimerStop`
- **AND** no leak occurs

#### Scenario: Loop destroyed before fire, Promise already dropped
- **WHEN** `Promise<void>::after(1000)` is called
- **AND** the returned promise is destroyed before the timer fires
- **AND** the host event loop is subsequently destroyed
- **THEN** `~TimerPromiseNode` (during Promise destruction) called
  `xTimerStop`, removing the timer from the heap
- **AND** `on_cancel_cb` is NOT invoked for this timer (it was
  already stopped)
- **AND** no leak occurs
