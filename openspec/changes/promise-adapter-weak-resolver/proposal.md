## Why

The current Promise system has two lifecycle safety gaps:

1. **PromiseResolver holds a raw pointer to AdapterPromiseNode.** If the Promise is destroyed before `resolve()` is called (e.g., a losing branch in `race()`), the pointer dangles — calling `resolve()` is a use-after-free.

2. **TimerPromiseNode duplicates poll/waker/resolved logic.** Its `m_fired` (atomic), `m_waker` (PromiseAtomicWaker), and `poll()` are identical to what a generalized adapter node would provide. Every future async-to-Promise bridge (HTTP, DNS, thread pool) would face the same duplication.

The fix is a unified Adapter + WeakResolver design: `AdaptedPromiseNode<T, Adapter>` owns an Adapter and shares a `ResolveState<T>` via `Arc`/`ArcWeak`. `WeakResolver<T>` holds an `ArcWeak` — safe to call after the node is destroyed. `TimerPromiseNode` becomes a thin `TimerAdapter` that only manages the `xTimer` handle.

## What Changes

- Add `ResolveState<T>` — shared heap object (value + waker + resolved flag), owned via `Arc<ResolveState<T>>`.
- Add `WeakResolver<T>` — replaces `PromiseResolver<T>`. Holds `ArcWeak<ResolveState<T>>`. `resolve()` calls `ArcWeak::upgrade()`; if the node is dead, `upgrade()` returns `None` and the call is silently dropped. Thread-safe.
- Add `AdaptedPromiseNode<T, Adapter>` — replaces `AdapterPromiseNode<T>`. Owns an `Adapter` and an `Arc<ResolveState<T>>`. `poll()` checks `resolved` + registers waker (generic, same for all adapters). Adapter's constructor receives `WeakResolver<T>&&` and starts the async operation; Adapter's destructor cancels it.
- Add `TimerAdapter` — replaces `TimerPromiseNode`. ~15 lines: owns `xTimer`, callback calls `m_resolver.resolve()`, destructor calls `xTimerStop`.
- Add `newAdaptedPromise<T, Adapter>(args...)` — factory that creates `AdaptedPromiseNode` and returns `Promise<T>`.
- Add `newPromiseAndResolver<T>()` — factory returning `{Promise<T>, WeakResolver<T>}` for manual resolve.
- Delete `AdapterPromiseNode<T>`, `TimerPromiseNode`, `PromiseResolver<T>`.
- Update `Promise<void>::after()` to use `AdaptedPromiseNode + TimerAdapter`.
- Update all existing tests: `PromiseResolver` → `WeakResolver`.
- Depends on PR #15 (Arc/ArcWeak) being merged.

## Capabilities

### New Capabilities
- `promise-adapter`: Generic Adapter pattern for bridging external async operations into Promise. `AdaptedPromiseNode<T, Adapter>` owns the Adapter, shares state via Arc, provides generic poll(). Adapter contract: constructor starts async op, destructor cancels, callback calls WeakResolver::resolve().
- `weak-resolver`: Safe resolver handle using ArcWeak. Replaces PromiseResolver. `resolve()` is thread-safe and silently drops if the Promise is already destroyed. Eliminates UAF in race() and other early-destruction scenarios.

### Modified Capabilities
- `promise-timer`: `TimerPromiseNode` replaced by `TimerAdapter` + `AdaptedPromiseNode`. Public API `Promise<void>::after(ms)` unchanged. Internal simplification: ~50 lines → ~15 lines.

## Impact

- **New files**: `promise_adapter.h` (ResolveState, WeakResolver, AdaptedPromiseNode, TimerAdapter, factories), `promise_adapter_test.cpp`
- **Modified files**: `promise.h` (remove PromiseResolver, update after()), `promise_node.h` (remove TimerPromiseNode, AdapterPromiseNode), existing test files (PromiseResolver → WeakResolver)
- **Deleted**: `AdapterPromiseNode<T>`, `TimerPromiseNode`, `PromiseResolver<T>` (replaced, not just removed)
- **Dependencies**: `arc.h` / `weak.h` from PR #15 (Arc, ArcWeak)
- **Non-goals**: Error/reject support (ExceptionOr), custom deleters, arena allocation, heterogeneous Adapter (single Adapter type per node)
