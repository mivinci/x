## 1. ResolveState + WeakResolver

- [ ] 1.1 Define `ResolveState<T>` in `promise_adapter.h`: `Option<T> value`, `AtomicPromiseWaker waker`, `std::atomic<bool> resolved{false}`
- [ ] 1.2 Define `WeakResolver<T>` in `promise_adapter.h`: holds `ArcWeak<ResolveState<T>>`, `resolve(T&&)` calls `upgrade()` → if Some, check `resolved` flag, set value, wake
- [ ] 1.3 Add void specialization `WeakResolver<void>`: `resolve()` takes no args, value type is `Void`
- [ ] 1.4 Add `is_pending()`: returns `upgrade().is_some() && !resolved`

## 2. AdaptedPromiseNode

- [ ] 2.1 Define `AdaptedPromiseNode<T, Adapter>` in `promise_adapter.h`: `Arc<ResolveState<T>> m_state`, `Adapter m_adapter`
- [ ] 2.2 Implement `poll(waker)`: if `resolved.load(acquire)` → return `Some(state.value)`; else register waker, re-check, return None
- [ ] 2.3 Constructor: create `Arc<ResolveState<T>>`, create `WeakResolver<T>` from `ArcWeak::downgrade()`, construct `Adapter(WeakResolver<T>&&, args...)`
- [ ] 2.4 Destruction order: `~Adapter()` (cancel op) then `~Arc<State>()` (drop strong ref) — guaranteed by member declaration order

## 3. TimerAdapter

- [ ] 3.1 Define `TimerAdapter` in `promise_adapter.h`: `xTimer m_handle`, `WeakResolver<void> m_resolver`
- [ ] 3.2 Constructor: `xTimerStart(callback, this, nullptr, ms, 0)` — callback calls `m_resolver.resolve()`
- [ ] 3.3 Destructor: `if (m_handle) xTimerStop(m_handle)` — synchronous on event loop thread
- [ ] 3.4 Delete move/copy (timer handle is non-transferable)

## 4. Factory Functions

- [ ] 4.1 `newAdaptedPromise<T, Adapter>(args...)` — creates `AdaptedPromiseNode`, returns `Promise<T>`
- [ ] 4.2 `newPromiseAndResolver<T>()` — creates `AdaptedPromiseNode<T, ManualAdapter<T>>` where ManualAdapter stores the WeakResolver, returns `{Promise<T>, WeakResolver<T>}`
- [ ] 4.3 Define `ManualAdapter<T>` — trivial adapter that just stores the WeakResolver (for the manual-resolve use case)

## 5. Migrate Promise::after

- [ ] 5.1 Change `Promise<void>::after(ms)` to use `newAdaptedPromise<void, TimerAdapter>(ms)`
- [ ] 5.2 Remove `TimerPromiseNode` from `promise_node.h`
- [ ] 5.3 Remove `AdapterPromiseNode` from `promise_node.h`

## 6. Migrate Existing API

- [ ] 6.1 Remove `PromiseResolver<T>` from `promise.h`
- [ ] 6.2 Remove `AdapterPromiseNode` references from `promise.h` friend declarations
- [ ] 6.3 Add `#include <xpp/promise_adapter.h>` to `promise.h` (or have users include it directly)

## 7. Tests

- [ ] 7.1 WeakResolver basic: resolve before wait → value correct
- [ ] 7.2 WeakResolver deferred: resolve after delay via timer → value correct
- [ ] 7.3 WeakResolver after Promise destroyed: destroy Promise, call resolve() → no crash
- [ ] 7.4 WeakResolver double resolve: second call silently dropped
- [ ] 7.5 WeakResolver cross-thread: resolve from worker thread → value correct
- [ ] 7.6 TimerAdapter basic: `after(ms)` resolves after ~ms
- [ ] 7.7 TimerAdapter cancel: destroy Promise before timer fires → timer stopped, no crash
- [ ] 7.8 TimerAdapter then chain: `after(ms).then(fn).wait()` works
- [ ] 7.9 newPromiseAndResolver: `{p, r} = newPromiseAndResolver<int>()`, `r.resolve(42)`, `p.wait() == 42`
- [ ] 7.10 newAdaptedPromise with custom adapter: verify Adapter constructor/destructor/resolve
- [ ] 7.11 Migrate all existing PromiseResolver tests to WeakResolver
- [ ] 7.12 Migrate all existing TimerPromiseNode/after tests (verify no regression)

## 8. Build & CI

- [ ] 8.1 Add `promise_adapter.h` to `libxpp/xpp/` (auto-discovered by CMake GLOB)
- [ ] 8.2 Add `promise_adapter_test.cpp` (auto-discovered by CMake GLOB)
- [ ] 8.3 Verify all existing Promise tests pass (no regressions)
- [ ] 8.4 Verify combinator tests (all/race) still pass with WeakResolver
- [ ] 8.5 Update `docs/libxpp/promise.md` with Adapter + WeakResolver section
