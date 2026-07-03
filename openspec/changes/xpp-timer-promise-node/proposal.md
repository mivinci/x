## Why

`Promise<void>::after(ms)` currently builds its timer by hand: heap-allocates
a `PromiseResolver<void>`, calls `xTimerStart` with a lambda that resolves
and `delete`s the resolver, and returns the associated `Promise`. This
has two correctness bugs:

1. **Use-after-free when Promise is destroyed before fire.** The
   `AdapterPromiseNode` is owned by the `Promise` (via `Own<>`), but the
   timer callback still holds a raw pointer to the resolver, which holds
   a raw pointer to the node. If the Promise drops before fire, the
   node is destroyed, the timer fires, and `resolve()` dereferences
   freed memory.

2. **Leak when loop is destroyed before fire.** If the event loop is
   torn down while the timer is still pending, libx recycles the timer
   struct but never had a way to release the resolver — it just leaked.
   PR #7 (`x-timer-on-cancel`) added the `on_cancel` hook that makes
   this fixable, but `Promise::after` still passes `NULL`.

Both bugs have the same root cause: ownership of the resolver/node is
decoupled from the timer's lifecycle. A dedicated `TimerPromiseNode`
that owns the `xTimer` handle — and uses `on_cancel` to learn when
libx reclaims the timer — closes the gap.

## What Changes

- Add `class TimerPromiseNode : public PromiseNode<void>` in
  `libxpp/xpp/promise_node.h`. It owns an `xTimer` handle, an
  `std::atomic<bool> m_fired` flag, and a `AtomicPromiseWaker`.
- `TimerPromiseNode` registers two libx callbacks at construction:
  - `fire_cb`: sets `m_fired=true`, wakes the waker
  - `on_cancel_cb`: nulls `m_handle`, sets `m_fired=true`, wakes the
    waker (invoked by libx on loop destroy — this is the first user
    of the `on_cancel` hook added in PR #7)
- `~TimerPromiseNode` calls `xTimerStop(m_handle)` **only if `m_fired`
  is still false**. If `m_fired` is true, either the timer fired
  (handle is dangling — libx already freed the struct) or `on_cancel`
  ran (handle was already nulled). In both cases the destructor does
  not touch `m_handle`.
- Rewrite `Promise<void>::after(ms)` to construct a `TimerPromiseNode`
  directly. Delete the `PromiseResolver<void>` + lambda dance. After
  this, `after()` is 3 lines.
- The `Promise<void>::after()` contract is unchanged from the user's
  perspective: returns a `Promise<void>` that resolves after `ms`.
- **Must be called within a WaitScope** (existing contract — already
  documented on `Promise::after`).

## Capabilities

### New Capabilities

_None — extends an existing capability._

### Modified Capabilities

- `promise-after-timer`: replace the "AdapterPromiseNode + resolver +
  lambda" implementation with a dedicated `TimerPromiseNode` that owns
  the timer handle and uses `on_cancel` for safe shutdown. The
  user-visible API (`Promise<void>::after(ms)`) is unchanged; the
  change is purely internal correctness.

## Impact

- **libxpp/xpp/promise_node.h**: new `TimerPromiseNode` class (≈40
  lines).
- **libxpp/xpp/promise.h**: `Promise<void>::after` simplified from
  ~10 lines to ~3 lines; no longer depends on `PromiseResolver<void>`
  or heap-allocating a resolver.
- **libxpp/xpp/promise_test.cpp** + **promise_deadlock_test.cpp**:
  existing tests should pass unchanged. New tests cover the two
  previously-broken scenarios (Promise destroyed before fire; loop
  destroyed before fire).
- **Dependencies**: requires `xTimerStart`'s `on_cancel` parameter
  (PR #7, merged). No new external dependencies.
- **ABI**: none — libxpp is header-only.
- **Performance**: one fewer heap allocation per `after()` call
  (the resolver is gone). The `TimerPromiseNode` is heap-allocated
  by `Own<>` as before.
