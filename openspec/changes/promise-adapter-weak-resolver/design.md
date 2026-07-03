## Context

The current `AdapterPromiseNode<T>` uses a raw pointer from `PromiseResolver` to the node. If the Promise is destroyed (e.g., a losing branch in `race()`), the pointer dangles. `TimerPromiseNode` duplicates the poll/waker/resolved logic that a generalized adapter would provide.

The new design uses `Arc<T>` / `ArcWeak<T>` (from PR #15) to share a `ResolveState<T>` between the node and the resolver. The node owns a strong reference; the resolver owns a weak reference. When the node is destroyed, the strong count drops to zero; the resolver's `upgrade()` returns `None`, and `resolve()` is silently dropped.

## Goals / Non-Goals

**Goals:**
- Eliminate UAF when Promise is destroyed before resolve()
- Generalize the adapter pattern so TimerPromiseNode and future bridges (HTTP, DNS, Work) share a common poll/waker/state implementation
- Thread-safe WeakResolver (resolve() callable from any thread)
- Use Arc/ArcWeak (not std::shared_ptr) — consistent with xpp design philosophy

**Non-Goals:**
- Error/reject (ExceptionOr) — deferred
- Arena allocation — deferred
- Custom deleters on Arc — not needed (ResolveState is self-contained)
- Adapter for cross-thread work (xWorkSubmit) — future, but the design supports it

## Decisions

### D1: ResolveState is a plain struct, not a PromiseNode

```cpp
template <class T>
struct ResolveState {
  Option<T>            value;
  PromiseAtomicWaker   waker;
  std::atomic<bool>    resolved{false};
};
```

ResolveState is the shared data between AdaptedPromiseNode and WeakResolver. It is NOT a PromiseNode — it has no poll() method. AdaptedPromiseNode owns an `Arc<ResolveState<T>>` and implements poll() by checking `resolved` + `waker`. WeakResolver owns an `ArcWeak<ResolveState<T>>` and implements resolve() by upgrading to Arc, setting value + resolved, and waking.

Rationale: keeping ResolveState separate from the node allows the state to outlive the node (which is the whole point). If ResolveState were a PromiseNode, its destructor would be tied to the node's lifetime.

### D2: Adapter receives WeakResolver by value (moved in)

```cpp
class TimerAdapter {
  WeakResolver<void> m_resolver;
public:
  TimerAdapter(WeakResolver<void>&& r, uint64_t ms)
    : m_resolver(std::move(r)) { ... }
};
```

The Adapter stores the WeakResolver. When the async callback fires, it calls `m_resolver.resolve(value)`. If the Adapter (and the node) are already destroyed, `resolve()` silently drops.

The Adapter's destructor cancels the async operation (e.g., `xTimerStop`). For same-thread operations (like timers), the cancel is synchronous and the callback can't fire after destruction. For cross-thread operations, the callback might fire after destruction — but WeakResolver handles this safely.

### D3: AdaptedPromiseNode owns Adapter as a member

```cpp
template <class T, class Adapter>
class AdaptedPromiseNode : public PromiseNode<T> {
  Arc<ResolveState<T>> m_state;
  Adapter              m_adapter;
  // ...
};
```

Member init order: `m_state` first (creates the Arc), then `m_adapter` (receives WeakResolver created from `m_state`). C++ guarantees members are initialized in declaration order, so `m_state` is ready when `m_adapter`'s constructor runs.

Destruction order: `m_adapter` first (cancels async op), then `m_state` (drops strong ref). If the async op was already cancelled synchronously, no callback can fire. If it wasn't (cross-thread), the callback's WeakResolver::resolve() will find `upgrade() == None` and drop safely.

### D4: WeakResolver replaces PromiseResolver, no compat shim

`PromiseResolver<T>` is deleted. All existing code and tests are updated to use `WeakResolver<T>`. The API is nearly identical:

```cpp
// Before:
auto r = PromiseResolver<int>::create();
auto p = r.promise();
r.resolve(42);

// After:
auto [p, r] = newPromiseAndResolver<int>();
r.resolve(42);
```

Rationale: a compat shim (typedef or wrapper) would hide the lifetime semantics change. Explicit migration is safer.

### D5: TimerAdapter passes `this` to timer callback

```cpp
TimerAdapter(WeakResolver<void>&& r, uint64_t ms)
  : m_resolver(std::move(r)) {
  m_handle = xTimerStart(
    [](void* a) { static_cast<TimerAdapter*>(a)->m_resolver.resolve(); },
    this, nullptr, ms, 0);
}
```

This is safe because `xTimerStop` is synchronous on the event loop thread — after it returns, the callback can't fire. The destructor calls `xTimerStop` before `m_resolver` is destroyed.

For the `on_cancel` (loop destroy) case: the callback is set to `nullptr` (no action needed — `m_resolver` will be destroyed with the node, and the WeakResolver will find `upgrade() == None`).

### D6: void specialization handled at ResolveState level

`ResolveState<void>` uses `Void` as the value type (via `FixVoid<void>::Type`). `WeakResolver<void>::resolve()` takes no arguments. `AdaptedPromiseNode<void, Adapter>` returns `Option<Void>` from poll(). This is consistent with the existing void handling in PromiseNode.

## Risks / Trade-offs

- **[Arc overhead]** Each resolve() does one `ArcWeak::upgrade()` (CAS loop). This is more expensive than the current raw pointer + atomic flag. Acceptable — resolve() is not a hot path.
- **[Extra heap allocation]** `Arc<ResolveState<T>>` is a separate allocation from the node. The current AdapterPromiseNode stores value/waker inline. Trade-off: the separate allocation is what enables safe lifecycle decoupling.
- **[Breaking change]** `PromiseResolver` removal breaks existing code. All tests and any downstream code must be updated.
- **[TimerAdapter on_cancel]** If the event loop is destroyed while a timer is pending, libx calls `on_cancel`. The TimerAdapter's `on_cancel` is `nullptr` — the timer handle is invalidated by libx, and the destructor checks `m_handle` before calling `xTimerStop`. The WeakResolver will find `upgrade() == None` if the callback somehow fires.
