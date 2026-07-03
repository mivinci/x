## 1. AllTuplePromiseNode

- [x] 1.1 Create `AllTuplePromiseNode<Ts...>` in `promise_combinators.h`: `std::tuple<Own<PromiseNode<Ts>>...>` children, `std::tuple<Option<FixVoid<Ts>::Type>...>` results, `m_remaining` counter
- [x] 1.2 Implement `poll()` using `std::index_sequence` + fold expression: poll each not-done child, collect `Some` results, return `Some(tuple)` when `m_remaining == 0`
- [x] 1.3 Implement `collect()` helper: `make_tuple(move(get<I>(m_results).unwrap())...)`
- [x] 1.4 Constructor takes `Promise<Ts>&&...`, extracts `m_node` from each via `_extract_node` friend

## 2. AllVoidPromiseNode

- [x] 2.1 Create `AllVoidPromiseNode<N>` in `promise_combinators.h`: `std::array<Own<PromiseNode<void>>, N>` children, `std::array<bool, N>` done flags, `m_remaining` counter
- [x] 2.2 Implement `poll()`: loop over children, poll not-done ones, return `Some(Void{})` when `m_remaining == 0`
- [x] 2.3 Constructor takes `Promise<void>&&...`, extracts nodes

## 3. RacePromiseNode

- [x] 3.1 Create `RacePromiseNode<T, N>` in `promise_combinators.h`: `std::array<Own<PromiseNode<T>>, N>` children
- [x] 3.2 Implement `poll()`: loop over all children, return first `Some` result
- [x] 3.3 Add void specialization `RacePromiseNode<Void, N>`: same logic, returns `Some(Void{})`
- [x] 3.4 Constructor takes `Promise<T>&&...`, extracts nodes

## 4. Free Functions

- [x] 4.1 Implement `xpp::all(Promise<Ts>...)` with SFINAE dispatch: all-void → `AllVoidPromiseNode`, else → `AllTuplePromiseNode`
- [x] 4.2 Add `static_assert(sizeof...(Ts) > 0)` for zero-argument rejection
- [x] 4.3 Implement `xpp::race(Promise<T> first, Promise<Rest>...)` with `static_assert` for homogeneous types
- [x] 4.4 Void overload of `race` dispatches to `RacePromiseNode<Void, N>` via `FixVoid<T>::Type`

## 5. Tests

- [x] 5.1 All immediate heterogeneous: `all(Promise<int>::resolve(1), Promise<string>::resolve("hi"))` → `tuple(1, "hi")`
- [x] 5.2 All immediate all-void: `all(Promise<void>::resolve(), Promise<void>::resolve())` → void
- [x] 5.3 All with void + value: `all(Promise<void>::resolve(), Promise<int>::resolve(7))` → `tuple(Void{}, 7)`
- [x] 5.4 All deferred via PromiseResolver + timer: verify waits for all, collects correct values
- [x] 5.5 All deferred all-void with timers: verify waits for slowest
- [x] 5.6 All with `then` chain: `all(...).then([](auto t) { ... })`
- [x] 5.7 Race immediate: first argument wins
- [x] 5.8 Race deferred: faster timer wins
- [x] 5.9 Race with timer timeout pattern: `race(fetch, Promise<void>::after(timeout))`
- [x] 5.10 Race void: `race(Promise<void>::after(10), Promise<void>::after(50))` → resolves at ~10ms
- [x] 5.11 Race destruction safety: verify `TimerPromiseNode` timer is stopped when race resolves before timer fires
- [x] 5.12 Race with `then` chain

## 6. Build & Integration

- [x] 6.1 Add `promise_combinators.h` to `libxpp/xpp/` (header-only, auto-discovered by CMake GLOB)
- [x] 6.2 Add `promise_combinators_test.cpp` to `libxpp/xpp/CMakeLists.txt` (auto-discovered by GLOB)
- [x] 6.3 Verify all existing Promise tests still pass (42 + 4 = 46, no regressions)
- [ ] 6.4 Update `docs/libxpp/promise.md` with `all`/`race` section (API, examples, comparison table)
