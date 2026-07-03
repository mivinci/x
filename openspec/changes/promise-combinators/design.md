## Context

xpp::Promise is built on `PromiseNode<T>` — a virtual interface with a single method:
```cpp
virtual Option<ValueType> poll(const PromiseWaker &waker) = 0;
```

The waker is a 16-byte trivially-copyable struct. When a child node is not ready, it stores a copy of the waker. When the async operation completes, the waker is fired, setting a `done` flag that causes `xEventLoopRun` to return, triggering a re-poll.

This design has two properties that make combinators straightforward:
1. **Waker is copyable** — multiple children can share the same waker; any one firing it wakes the parent.
2. **Re-polling is idempotent for done children** — once a node returns `Some`, the parent tracks it as done and skips re-polling (the one-shot contract forbids re-polling anyway).

## Goals / Non-Goals

**Goals:**
- `all` with heterogeneous types via `std::tuple`
- `all` with all-void → `Promise<void>` (not `tuple<Void, Void, ...>`)
- `race` homogeneous (same type)
- Zero changes to existing Promise/Node/Waker API
- Compile-time N (variadic templates, no `std::vector`)

**Non-Goals:**
- Heterogeneous `race` (needs `std::variant`, low value)
- `cancel()` virtual on PromiseNode (deferred; destroy semantics suffice for now)
- Variadic `select!` with branch arms (needs pattern matching, future work)

## Decisions

### D1: Free functions, not static methods

`all` and `race` are free functions in namespace `xpp`, not static methods on `Promise<T>`. Rationale: `all` is heterogeneous (not bound to a single T), so it can't be `Promise<T>::all(...)`. Free functions are the natural fit:

```cpp
namespace xpp {
  template <class... Ts> auto all(Promise<Ts>...);
  template <class T, class... Rest> Promise<T> race(Promise<T>, Promise<Rest>...);
}
```

### D2: all returns tuple; all-void returns void via if constexpr

```cpp
template <class... Ts>
auto all(Promise<Ts>... promises) {
  if constexpr ((std::is_same_v<Ts, void> && ...)) {
    return all_void_impl<sizeof...(Ts)>(...);
  } else {
    return all_tuple_impl<Ts...>(...);
  }
}
```

Rationale: `tuple<Void, Void, Void>` is ergonomically terrible for the common "wait for N completions" case. `if constexpr` dispatches at compile time with zero runtime cost.

### D3: AllTuplePromiseNode uses std::tuple + fold expressions

Internal storage:
```cpp
std::tuple<Own<PromiseNode<Ts>>...>               m_children;
std::tuple<Option<typename FixVoid<Ts>::Type>...> m_results;
size_t m_remaining = sizeof...(Ts);
```

Polling uses `std::index_sequence` + C++17 fold expression:
```cpp
template <size_t... Is>
void poll_all(const PromiseWaker &w, std::index_sequence<Is...>) {
  (poll_one<Is>(w), ...);
}
```

Rationale: This is the standard C++17 technique for compile-time tuple iteration. No runtime loops, no virtual dispatch per child beyond the existing `poll()` call.

### D4: Race is homogeneous with static_assert

```cpp
template <class T, class... Rest>
Promise<T> race(Promise<T> first, Promise<Rest>... rest) {
  static_assert((std::is_same_v<Rest, T> && ...), "race requires homogeneous types");
  ...
}
```

Rationale: Heterogeneous race would return `std::variant<Ts...>`, requiring `std::visit` at the call site. The typical race use cases (N CDNs, promise + timer) are homogeneous. If heterogeneous race is needed later, it can be added as a separate function.

### D5: Race destruction — caller manages resolver lifetimes

When race resolves, N-1 losing children are destroyed. Their destructors run:
- `TimerPromiseNode`: stops the timer ✓
- `AdapterPromiseNode`: destroyed, but `PromiseResolver` may still hold a raw pointer ⚠

This is the same destroy-before-resolve risk that exists for single Promises. Race amplifies it (N-1 losers instead of 0), but the contract is unchanged: the caller must ensure resolvers don't outlive their Promises.

**Alternative considered**: Add `virtual void cancel()` to `PromiseNode`. `AdapterPromiseNode::cancel()` would set an atomic flag, and `resolve()` would check it. Rejected for now — adds vtable entry to every node and complexity to the hot path. Can be added later if UAF becomes a real problem.

### D6: AllVoidPromiseNode uses std::array, not std::tuple

For the all-void case, all children are `PromiseNode<void>`. Using `std::array<Own<PromiseNode<void>>, N>` is simpler than `std::tuple` and allows a runtime loop:

```cpp
template <size_t N>
class AllVoidPromiseNode : public PromiseNode<void> {
  std::array<Own<PromiseNode<void>>, N> m_children;
  std::array<bool, N> m_done{};
  size_t m_remaining = N;
};
```

Rationale: When all types are the same, a runtime loop over an array is simpler than fold expressions. The compiler may unroll it anyway.

## Risks / Trade-offs

- **[Race UAF on losing branches]** → Mitigated by documentation; same contract as existing Promise destroy semantics. Future: add `cancel()` if needed.
- **[Template bloat]** → Each `all<T1, T2, T3>` instantiation generates a unique node type. Acceptable — the number of distinct type combinations is small in practice.
- **[N=0 edge case]** → `all()` with zero arguments is ill-formed. Add `static_assert(sizeof...(Ts) > 0)`.
- **[Move-only types in tuple]** → `Option<T>` supports move-only types. `collect()` uses `std::move` on unwrap.
