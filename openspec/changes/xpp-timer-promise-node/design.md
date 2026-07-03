## Context

`Promise<void>::after(ms)` is the only place in libxpp that bridges from
libx's C timer API into the Promise system. Today it does so by hand:

```cpp
inline Promise<void> Promise<T>::after(uint64_t ms) {
  auto *resolver = new PromiseResolver<void>(PromiseResolver<void>::create());
  auto  promise  = resolver->promise();
  xTimerStart(
    [](void *arg) {
      auto *r = static_cast<PromiseResolver<void> *>(arg);
      r->resolve();
      delete r;
    },
    resolver, NULL, ms, 0);
  return promise;
}
```

Ownership of the `AdapterPromiseNode` lives in the `Promise` (via
`Own<>`). Ownership of the `resolver` lives in the timer callback
(via `delete r`). These two ownerships are decoupled, which produces
two bugs:

1. **UAF on early Promise destruction** — `~Promise` deletes the
   node, but the timer still fires and calls `resolver->resolve()`
   → `m_node->resolve()` reads freed memory.
2. **Leak on loop destroy before fire** — libx reclaims the timer
   struct but has no path to release the resolver. (PR #7's
   `on_cancel` hook now provides this path, but `after()` passes
   `NULL`.)

PR #7 added `on_cancel` to `xTimerStart`. This change uses it.

**Stakeholders / constraints:**
- Must not change the user-visible `Promise<void>::after(ms)` signature.
- Must keep `Promise<void>::after` callable from any WaitScope thread
  (existing contract).
- Must not regress existing tests or benchmarks.
- No new external dependencies.

## Goals / Non-Goals

**Goals:**
- Fix the UAF when `Promise<void>::after(ms)` is dropped before fire
- Fix the resolver leak when the host loop is destroyed before fire
- Preserve the existing user-visible API (`Promise<void>::after(ms)`)
- Remove one heap allocation per `after()` call (the resolver)
- Become the first in-tree consumer of `xTimerStart`'s `on_cancel`
  hook — validating that the API design works for real consumers

**Non-Goals:**
- Adding `Promise<T>::after(ms, value)` for non-void `T` — out of
  scope. If needed, compose with `then()`.
- Adding `Promise<void>::interval(ms)` for repeating timers — out of
  scope. Separate change.
- Generalizing `TimerPromiseNode` to take a `PromiseResolver<T>` for
  external resolve — out of scope. The node is for self-contained
  timer-driven resolution only.
- Fixing `xEventLoopDestroy`'s lack of drain for offload `xWork`
  items — unrelated, separate change.

## Decisions

### D1: New `TimerPromiseNode` class, not a refactor of `AdapterPromiseNode`

**Decision:** Add a new `class TimerPromiseNode : public PromiseNode<void>`
in `promise_node.h`. Do not modify `AdapterPromiseNode`.

**Why:** `AdapterPromiseNode` is a general-purpose bridge for external
resolution (used by `PromiseResolver<T>`). Its semantics are "someone
out there will eventually call `resolve()`". `TimerPromiseNode` is a
specialized node whose semantics are "I own a timer, I resolve when it
fires". Mixing the two would muddy the adapter's contract.

**Alternatives considered:**
- Add a `timer_handle` field to `AdapterPromiseNode` and a "mode" enum
  → rejected — violates single-responsibility, complicates the
  adapter's already-tricky thread-safety contract.
- Subclass `AdapterPromiseNode` → rejected — the adapter is `final`.

### D2: `TimerPromiseNode` owns the `xTimer` handle

**Decision:** `TimerPromiseNode` stores `xTimer m_handle` and calls
`xTimerStart` in its constructor. The handle is owned exclusively by
the node — no external resolver, no separate resolver object.

**Why:** Centralizes ownership. The node's lifetime is the timer's
lifetime. When the node is destroyed (via `~Promise` → `~Own<>`),
the destructor calls `xTimerStop` if the timer hasn't fired. This
fixes the UAF: there's no longer a separate resolver that can be
called after the node dies.

**Alternatives considered:**
- Keep the resolver pattern but add an "I'm dead" flag → rejected —
  more moving parts, two heap allocations instead of one.
- External timer handle owned by the caller → rejected — breaks the
  Promise abstraction (callers shouldn't manage timer state).

### D3: Use `m_fired` flag (atomic) to gate destructor's `xTimerStop`

**Decision:** `~TimerPromiseNode` calls `xTimerStop(m_handle)` **only
if `m_fired` is still `false`**.

**Why:** After fire, libx recycles the timer struct via its freelist
(`timer_free`). The `m_handle` pointer becomes dangling — calling
`xTimerStop` on it is UAF. The `m_fired` flag is set in `fire_cb`
before the node returns control to libx's dispatcher, so by the time
the destructor runs (on the same thread — see D5), `m_fired` reflects
whether the handle is still valid.

The same flag is reused for `on_cancel_cb`: when libx invokes
`on_cancel` on loop destroy, the callback sets `m_fired=true` and
nulls `m_handle`. The destructor then sees `m_fired=true` and skips
`xTimerStop`.

**Memory ordering:** `m_fired` is `std::atomic<bool>`. `fire_cb` and
`on_cancel_cb` use `store(true, memory_order_release)`. The destructor
uses `load(memory_order_acquire)`. This pairs the release-acquire
happens-before, ensuring the destructor's check sees the latest
value. (In practice the contract requires single-thread — see D5 —
so the ordering is belt-and-suspenders.)

**Alternatives considered:**
- `std::atomic<xTimer>` with CAS in destructor → rejected — overkill
  for single-thread contract, adds atomic overhead on every poll.
- `weak_handle`-style validity query on libx side → rejected — would
  require new libx API surface; `on_cancel` already provides the
  "libx reclaimed the timer" signal.

### D4: `on_cancel_cb` is mandatory for `TimerPromiseNode`

**Decision:** `TimerPromiseNode` always passes a non-NULL `on_cancel`
to `xTimerStart`. It does not use `NULL`.

**Why:** Without `on_cancel`, when the host loop is destroyed before
fire, libx silently recycles the timer struct. `TimerPromiseNode`'s
`m_handle` becomes dangling. The next `poll()` after loop destroy
would try to `xTimerStop(m_handle)` in the destructor and UAF.

`on_cancel_cb` is the notification that lets the node mark itself
"fired" (really, "cancelled by libx") and null its handle.

This makes `TimerPromiseNode` the first real consumer of the
`on_cancel` API added in PR #7 — validating the design.

**Alternatives considered:**
- Pass `NULL` and detect dangling handle some other way → rejected —
  there's no other detection mechanism in libx.

### D5: `TimerPromiseNode` must be destroyed on the WaitScope thread

**Decision:** Document the contract: `~TimerPromiseNode` (and thus
`~Promise<void>` for a promise returned by `after()`) must run on
the same thread that owns the WaitScope. This matches the existing
`Promise::wait()` contract.

**Why:** `fire_cb` runs on the loop thread (libx dispatches timers
there). `on_cancel_cb` runs on the loop thread (during
`xEventLoopDestroy`). The destructor must serialize with both —
either the destructor runs before the callback (timer still pending,
stop is safe), or after (flag is set, destructor skips stop). If
the destructor ran on a different thread, it could race with
`fire_cb` mid-execution.

In practice this is already the contract for all of libxpp's Promise
operations. This decision just makes it explicit for `after()`.

**Alternatives considered:**
- Make destructor thread-safe with a mutex → rejected — adds overhead
  to every Promise destruction, and the rest of the Promise API
  isn't thread-safe for destruction either. Inconsistent.
- Use `AtomicPromiseWaker`'s 2-bit state machine for full lock-free
  coordination → rejected — overkill; the single-thread contract is
  already required for `wait()`.

### D6: `fire_cb` and `on_cancel_cb` are separate static functions

**Decision:** `TimerPromiseNode` registers two distinct static
callbacks with `xTimerStart`:
- `fire_cb(void *arg)`: sets `m_fired=true`, wakes waker
- `on_cancel_cb(void *arg)`: nulls `m_handle`, sets `m_fired=true`,
  wakes waker

**Why:** libx's `xTimerFunc` signature is `void(*)(void *)` — no
"reason" parameter. The two paths have different side effects
(`on_cancel` must null the handle; fire doesn't need to since the
handle is already dangling from libx's perspective). Splitting them
keeps each callback minimal and readable.

**Alternatives considered:**
- Single callback with a discriminator → rejected — would require
  extending `xTimerFunc`'s signature, which is a much larger
  breaking change than this change needs.
- Inline the logic via lambdas → rejected — `xTimerFunc` is a C
  function pointer; capturing lambdas don't convert. The callbacks
  must be free/static functions anyway.

### D7: `Promise<void>::after` becomes a 3-line function

**Decision:** Rewrite `after()` to:

```cpp
template <class T>
template <class V, class, class>
inline Promise<void> Promise<T>::after(uint64_t ms) {
  return Promise<void>(Own<PromiseNode<void>>(new TimerPromiseNode(ms)));
}
```

**Why:** The old implementation was 10 lines and allocated two heap
objects (the resolver and its `AdapterPromiseNode`). The new
implementation allocates one heap object (`TimerPromiseNode`) and
is 3 lines. Less code, fewer allocations, no UAF.

**Alternatives considered:**
- Keep the resolver pattern, just add `on_cancel` → rejected —
  doesn't fix the UAF (resolver still calls into a node that may
  be dead).

## Risks / Trade-offs

- **[Risk] Single-thread destruction contract is implicit**
  - Mitigation: Add a docstring to `Promise::after` and to
    `TimerPromiseNode` itself stating the contract. Existing tests
    all destroy on the WaitScope thread — no behavior change.

- **[Risk] `m_fired` atomic adds a small overhead on every `poll()`**
  - Mitigation: `acquire` load on a contended atomic is ~1ns on
    modern ARM64/x86. `after()` is not a hot path; the waker
    already uses atomics. Negligible.

- **[Risk] `on_cancel_cb` runs during `xEventLoopDestroy` — what if
  it calls back into the loop?**
  - Mitigation: `on_cancel_cb` only touches `TimerPromiseNode`
    member fields (atomics + waker). It does not call any libx API.
    Documented in PR #7's contract.

- **[Trade-off] `TimerPromiseNode` is hardcoded to `PromiseNode<void>`**
  - Accepted: `after()` only returns `Promise<void>`. Non-void
    composition uses `then()`. Adding `<T>` support would complicate
    the node for no current caller.

- **[Trade-off] `TimerPromiseNode` cannot be externally resolved
  (unlike `AdapterPromiseNode`)**
  - Accepted: it's a specialized timer node, not a general adapter.
    If external resolution is needed, use `PromiseResolver<T>::create()`
    and compose with a timer separately.
