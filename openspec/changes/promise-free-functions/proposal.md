## Why

The Promise API is currently split: `async`, `all`, `race`, `yield` are free functions, but `resolve`, `after`, `work`, `defer`, `adapt` are static methods on `Promise<T>`. This inconsistency is confusing — users must remember which is which. Free functions are shorter, support type deduction, and read more naturally: `resolve(42)` vs `Promise<int>::resolve(42)`.

## What Changes

- Add free function `resolve(v)` → `Promise<T>` (T deduced from v). Void resolve is already covered by `yield()`.
- Add free function `after(ms)` → `Promise<void>`.
- Add free function `defer(fn)` → `Promise<T>` (T deduced from fn return type).
- Add free function `work(fn)` → `Promise<T>` (T deduced from fn return type).
- Add free function `adapt<T, Adapter>(args...)` → `Promise<T>` (both T and Adapter explicit).
- Remove static methods `Promise<T>::resolve()`, `Promise<void>::after()`, `Promise<T>::defer()`, `Promise<T>::work()`, `Promise<T>::adapt()` — no backward compat wrappers.
- `Promise<T>` becomes a pure consumer type: `then()`, `wait()`, `discard()`, `operator co_await()`, `operator bool()`.
- Update all tests and docs to use free functions.

## Capabilities

### New Capabilities
- `promise-free-functions`: Unified free-function API for Promise factories. All creation functions (`resolve`, `after`, `defer`, `work`, `adapt`, `async`, `all`, `race`, `yield`) are free functions in namespace `xpp`.

### Modified Capabilities
- `promise-coroutine`: `operator co_await()` stays on `Promise<T>` (it's a consumer method, not a factory). No change needed.

## Impact

- **New functions**: `resolve`, `after`, `defer`, `work`, `adapt` in `promise.h`
- **Removed**: `Promise<T>::resolve()`, `Promise<void>::after()`, `Promise<T>::defer()`, `Promise<T>::work()`, `Promise<T>::adapt()` — no deprecated wrappers
- **Modified**: all test files, docs
- **Breaking change**: existing code using static methods must migrate to free functions
- **Non-goals**: none — clean break
