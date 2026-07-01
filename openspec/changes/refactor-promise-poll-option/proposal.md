## Why

The current `PromiseNode<T>` interface splits readiness and value extraction into `poll(Waker) → bool` + `take() → T`, creating a two-step protocol with implicit state dependencies. The implementation also carries dead code (`SpawnTaskBase`, `Schedule` virtual hierarchy) from a runtime layer that libxpp doesn't have, and `Promise::wait()` references an undeclared member variable (`m_done`). Simplifying `poll` to return `Option<T>` eliminates `take()`, removes the state dependency, and shrinks the codebase by ~40%.

## What Changes

- **BREAKING**: `PromiseNode<T>::poll` now returns `Option<ValueType>` instead of `bool`. `Some(value)` = ready, `None` = pending.
- **BREAKING**: `PromiseNode<T>::take()` removed. The value is returned directly from `poll`.
- **BREAKING**: `Waker` simplified from `(Schedule*, SpawnTaskBase*)` pair to `(void(*)(void*), void*)` function-pointer + arg. No more virtual `Schedule` base class.
- Removed `SpawnTaskBase` (join protocol, ~70 lines) — dead code from the runtime layer.
- Removed `Schedule` virtual hierarchy (~30 lines) — replaced by Waker's inline function pointer.
- Removed `SyncWaitSchedule` / `CoroWakeSchedule` — replaced by `Waker::sync_wait()` static factory.
- Removed `#if XPP_HAS_COROUTINES` blocks (coroutine support deferred to a future change).
- `Promise<T>::wait()` rewritten to use a local `bool done` flag (fixes the undeclared `m_done` bug).
- `ChainPromiseNode` simplified: uses `m_inner != nullptr` instead of `Step1`/`Step2` enum state machine.
- `AtomicWaker` retained (needed by `AdapterPromiseNode` for thread-safe resolve/poll).
- Node naming preserved: `ImmediatePromiseNode`, `TransformPromiseNode`, `ChainPromiseNode`, `AdapterPromiseNode`, `YieldPromiseNode`.

## Capabilities

### New Capabilities
- `promise-poll-option`: The simplified Promise/PromiseNode interface where `poll` returns `Option<T>` instead of `bool`, eliminating `take()` and the two-step poll-then-take protocol.

### Modified Capabilities
<!-- None — no existing spec describes Promise behavior. -->

## Impact

- `libxpp/xpp/promise_node.h`: Full rewrite (~645 lines → ~300 lines)
- `libxpp/xpp/promise.h`: Rewrite (~427 lines → ~250 lines)
- `libxpp/xpp/promise_test.cpp`: New test file (not ported from libx++; written fresh for the simplified API)
- No changes to `libx/` (C library) or `libdlproxy/`
- No changes to `event.h` or other xpp infrastructure
- Existing `event_test.cpp`, `box_test.cpp`, `own_test.cpp`, etc. unaffected
