## Context

Currently `Promise<T>` has both consumer methods (`then`, `wait`, `discard`) and static factory methods (`resolve`, `after`, `defer`, `work`, `adapt`). Meanwhile `async`, `all`, `race`, `yield` are already free functions. This split is inconsistent.

## Goals / Non-Goals

**Goals:**
- All factory functions as free functions in `xpp`
- Type deduction where possible (`resolve(42)`, `work(fn)`, `defer(fn)`)
- `adapt<T, Adapter>(args)` requires both template params (T can't be deduced)
- Keep old static methods as deprecated wrappers (not removed)
- Update all tests and docs

**Non-Goals:**
- Remove deprecated static methods (future cleanup)
- Change `then()`, `wait()`, `discard()`, `operator co_await()` — these stay as member methods
- Change `async`, `all`, `race`, `yield` — already free functions

## Decisions

### D1: Free functions deduce T from arguments

```cpp
resolve(42)              // → Promise<int>, T = int from argument
after(100)               // → Promise<void>, always void
defer([] { return 42; }) // → Promise<int>, T = int from return type
work([] { return 42; })  // → Promise<int>, T = int from return type
```

`adapt` can't deduce T (Adapter constructor doesn't expose it), so both are explicit:
```cpp
adapt<Response, HttpAdapter>(url)  // → Promise<Response>
```

### D2: `resolve()` for void is `yield()`

No `resolve()` without arguments. `yield()` already returns `Promise<void>` resolved immediately. No duplication.

### D3: Static methods removed entirely

No deprecated wrappers. Clean break. `Promise<T>` becomes a pure consumer type with no static factory methods. Users migrate all at once — the codebase is small enough.

### D4: Free functions defined in promise.h

No new header needed. The free functions are small (1-3 lines each) and go at the bottom of `promise.h` alongside the existing `async()`, `yield()` definitions.

### D5: `resolve` takes by value, moves internally

```cpp
template <class T>
Promise<T> resolve(T v) {
    return Promise<T>(Own<_::PromiseNode<T>>(
        new _::ImmediatePromiseNode<T>(std::move(v))));
}
```

Simple, no `&&` overloads needed. The value is moved once into the node.

## Risks / Trade-offs

- **[Name collision]** `resolve`, `after`, `work`, `defer` are generic names. In namespace `xpp`, they're scoped. Users who `using namespace xpp` might see conflicts. Acceptable — same risk as `yield`, `all`, `race` which already exist.
- **[Breaking change]** Static methods removed entirely. All call sites must migrate to free functions. The codebase is small enough for a clean break.
- **[adapt requires two template params]** `adapt<T, Adapter>(args)` is slightly more verbose than `Promise<T>::adapt<Adapter>(args)`. But consistent with `async<T>()`.
