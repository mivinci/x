## Why

The current Promise API uses `.then()` chains for composition. For multi-step async flows this leads to callback nesting and loses the linear readability of synchronous code. C++20 coroutines solve this — `co_await` and `co_return` let users write async code that looks synchronous, while the existing poll-based PromiseNode infrastructure drives execution under the hood.

## What Changes

- Add `Promise<T>::promise_type` (C++20 coroutine promise) so functions returning `Promise<T>` can be coroutines. No `Task<T>` wrapper — `Promise<T>` IS the coroutine return type.
- Add `Promise<T>::operator co_await()` so `co_await promise` works inside any coroutine.
- Add `CoroutinePromiseNode<T>` — a `PromiseNode<T>` that drives a coroutine via `poll()`. It resumes the coroutine, tracks `co_await` targets via type-erased `AwaitState`, and stores the result when the coroutine `co_returns`.
- Add `PromiseAwaiter<T>` — the awaitable returned by `operator co_await()`. Extracts the node from the Promise, stores it in the current coroutine's `CoroutinePromiseNode` for polling.
- All coroutine code is behind `#if XPP_HAS_COROUTINES` (already defined in `compiler.h`). C++17 users are unaffected.
- New file: `promise_coroutine.h` (included at the end of `promise.h`, conditionally).
- New test: `promise_coroutine_test.cpp` (C++20 target).

## Capabilities

### New Capabilities
- `promise-coroutine`: C++20 coroutine support for `Promise<T>`. Functions returning `Promise<T>` can use `co_await` and `co_return`. `Promise<T>` is both the coroutine return type and an awaitable.

### Modified Capabilities
_(none — existing C++17 API unchanged; coroutine support is purely additive behind `#if XPP_HAS_COROUTINES`)_

## Impact

- **New files**: `libxpp/xpp/promise_coroutine.h`, `libxpp/xpp/promise_coroutine_test.cpp`
- **Modified files**: `libxpp/xpp/promise.h` (conditional `#include <xpp/promise_coroutine.h>` at the end), `libxpp/xpp/CMakeLists.txt` (C++20 test target)
- **No changes** to: `promise_adapter.h`, `promise_node.h`, `promise_waker.h`, `promise_combinators.h`, existing tests
- **Dependencies**: `<coroutine>` (C++20 standard library), `XPP_HAS_COROUTINES` feature macro (already in `compiler.h`)
- **Non-goals**: Exception propagation via `ExceptionOr` (deferred), `co_yield` (not needed for Promise), custom allocator support, symmetric transfer optimization
