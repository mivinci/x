## Why

xpp::Promise currently supports sequential composition via `.then()` and auto-flattening via `ChainPromiseNode`, but lacks combinators for concurrent composition — waiting for multiple promises in parallel (`all`) or taking the first result (`race`). This limits real-world usage where N async operations need to run concurrently (e.g., prefetch N segments, fetch from multiple CDNs, implement timeout via racing a promise against a timer).

## What Changes

- Add `xpp::all(Promise<Ts>...)` free function: variadic template, heterogeneous types, returns `Promise<tuple<FixVoid<Ts>::Type...>>`. All-void case returns `Promise<void>` via `if constexpr` dispatch.
- Add `xpp::race(Promise<T>, Promise<T>...)` free function: homogeneous, returns first resolved `Promise<T>`.
- Add 3 new PromiseNode implementations:
  - `AllTuplePromiseNode<Ts...>` — heterogeneous all, uses `std::tuple` + fold expressions + `std::index_sequence` to poll N children and collect results
  - `AllVoidPromiseNode<N>` — all-void special case, compile-time array of children, just counts remaining
  - `RacePromiseNode<T, N>` — polls all children, returns first `Some`
- Add void specializations for `RacePromiseNode`.
- Tests covering: immediate all, deferred all, mixed void/value all, all-void, race with immediate winner, race with deferred winner, race with timer (timeout pattern), destruction safety.

## Capabilities

### New Capabilities
- `promise-all`: Combinator that waits for all input promises to resolve, collecting results into a tuple (heterogeneous) or completing when all void.
- `promise-race`: Combinator that resolves with the first ready promise, discarding the rest.

### Modified Capabilities
_(none — existing Promise<T>, PromiseNode, and PromiseWaker APIs are unchanged)_

## Impact

- **New files**: `libxpp/xpp/promise_combinators.h` (all/race free functions + 3 node types), `libxpp/xpp/promise_combinators_test.cpp`
- **Modified files**: `libxpp/xpp/CMakeLists.txt` (new test target)
- **No changes** to: `promise.h`, `promise_node.h`, `promise_waker.h`, `option.h`, `void.h`, `own.h`, `event.h`
- **Dependencies**: Uses `std::tuple`, `std::index_sequence`, C++17 fold expressions — all already required by the codebase.
- **Non-goals**: Heterogeneous `race` (would need `std::variant` return, deferred); `cancel()` mechanism for losing race branches (documented as caller responsibility, same as existing Promise destroy semantics).
