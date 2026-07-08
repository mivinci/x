# Combinators: all and race

[← Promise](README.md)

## When to Use

You need to wait for multiple Promises concurrently (`all`) or take the first result (`race`).

## `xpp::all(Promise<Ts>...)`

Waits for all input promises, collecting results into a `std::tuple`.

- **Heterogeneous**: each promise may have a different type
- **All-void**: returns `Promise<void>` (not `Promise<tuple<Void, Void, ...>>`)
- **Zero arguments**: rejected by `static_assert`

```cpp
#include <xpp/promise_combinators.h>

// Heterogeneous — returns tuple
auto [status, body] = xpp::all(
    fetch_status(url),   // Promise<int>
    fetch_body(url)      // Promise<std::string>
).await();

// All-void — returns void
xpp::all(
    prefetch(0),
    prefetch(1),
    prefetch(2)
).then([] { start_playback(); }).await();

// With void + value — Void in tuple, ignored
auto [_, val] = xpp::all(
    yield(),
    resolve(42)
).await();
// val == 42
```

## `xpp::race(Promise<T> first, Promise<Rest>...)`

Resolves with the first ready promise. All losing branches are destroyed.

- **Homogeneous**: all promises must have the same type `T`
- **Void support**: `race(Promise<void>...)` returns `Promise<void>`

```cpp
// Timeout pattern
auto result = xpp::race(
    fetch_async(url),                                        // Promise<int>
    xpp::after(5000).then([] { return -1; })  // timeout
).await();

// N CDNs, take fastest
auto fastest = xpp::race(fetch(cdn1), fetch(cdn2), fetch(cdn3)).await();

// Void race
xpp::race(after(10), after(50)).await();
// resolves at ~10ms
```

## How Waker Sharing Works

Both `all` and `race` pass the **same** `PromiseWaker` to all children. When any child fires the waker (sets `*done = true`), `wait()` re-polls the parent node.

Re-polling is safe because the parent tracks which children are done:

- `AllTuplePromiseNode`: checks `Option::is_some()` per child
- `RacePromiseNode`: returns on first `Some`, remaining children destroyed

## `wait()` uses `X_RUN_ONCE`

`Promise::wait()` runs the event loop with `X_RUN_ONCE` (one iteration per call) rather than `X_RUN_DEFAULT`. This is critical for `race`: with two timers, the faster timer sets `done = true`, but the slower timer keeps the loop alive. `X_RUN_ONCE` returns after each event, letting `wait()` re-check `done` immediately.

## Destruction Safety for `race`

When `race` resolves, N-1 losing children are destroyed:

| Node | Destructor behavior |
| ------ | --------------------- |
| `TimerAdapter` | Calls `xTimerStop` if not yet fired |
| `AdapterPromiseNode` | Destroyed — `PromiseResolver::resolve()` safely drops via `ArcWeak` |
| `ImmediatePromiseNode` | No cleanup needed |
| `TransformPromiseNode` | Destroys dependency chain |

With `ArcWeak`-based `PromiseResolver`, destroying an `AdapterPromiseNode` is safe — `resolve()` finds `upgrade() == None` and silently drops. No UAF.

## Comparison with Other Languages

| Feature | xpp | JS | Rust | folly |
| --------- | ----- | ----- | ------ | ------- |
| `all` | `all(p1, p2)` → `tuple` | `Promise.all` → `array` | `try_join` → `tuple` | `collect` → `vector` |
| `race` | `race(p1, p2)` → first | `Promise.race` → first | `select` → first | `any` → first |
| Heterogeneous `all` | Yes (tuple) | No | Yes (tuple) | No |
| Loser cleanup | Destructor | GC | `Drop` | Destructor |
| Executor needed | No | Yes | Yes | Yes |
