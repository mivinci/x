# Internals

[← Promise](README.md)

## PromiseNode Hierarchy

All async computations implement `PromiseNode<T>` — a virtual interface with one method:

```cpp
template <class T> class PromiseNode {
  using ValueType = typename FixVoid<T>::Type;
  virtual Option<ValueType> poll(const PromiseWaker &waker) = 0;
};
```

- `Some(value)` = ready, value extracted in the same call
- `None` = pending, waker stored for later notification
- **One-shot**: once `Some` is returned, `poll()` must never be called again

### Node Types

| Node Type | Purpose | poll() Behavior |
| ----------- | --------- | ----------------- |
| `ImmediatePromiseNode<T>` | `Promise::resolve(v)` | Returns `Some(v)` — ignores waker |
| `TransformPromiseNode<U, T, F>` | `.then(fn)` | Polls dependency; if `Some`, applies `fn` |
| `ChainPromiseNode<T>` | Auto-flatten `Promise<Promise<T>>` | Polls outer; when ready, switches to inner |
| `AdapterPromiseNode<T, Adapter>` | Generic adapter pattern | Polls `ResolveState`: check resolved → register waker → double-check |
| `ManualResolveNode<T>` | `async<T>()` factory | Same poll logic, no Adapter |
| `YieldPromiseNode` | `yield()` | Returns `Some(Void{})` |
| `AllTuplePromiseNode<Ts...>` | `all()` combinator | Polls all children, collects tuple when all done |
| `AllVoidPromiseNode<N>` | `all()` all-void | Countdown, returns `Some(Void{})` when all done |
| `RacePromiseNode<T, N>` | `race()` combinator | Returns first `Some`, destroys losers |

### TransformPromiseNode

Four partial specializations handle the void-unit-type mapping: `T→U`, `void→U`, `T→void`, `void→void`. Uses `_voidwrap::call` / `_voidwrap::call1` SFINAE helpers.

### ChainPromiseNode

Uses `m_inner != nullptr` as a state machine: `nullptr` = polling outer, non-null = polling inner. No enum, no extra branch.

## poll_state() — Shared Poll Logic

`AdapterPromiseNode` and `ManualResolveNode` share the same poll implementation via `poll_state()`:

```cpp
template <class T>
Option<T> poll_state(ResolveState<T> &s, const PromiseWaker &waker) {
    if (s.resolved.load(std::memory_order_acquire))
        return std::move(s.value);           // Fast path
    s.waker.register_waker(waker);           // May race with resolve
    if (s.resolved.load(std::memory_order_acquire)) {
        s.waker.wake();                      // Self-wake
        return std::move(s.value);
    }
    return none;
}
```

Void specialization returns `Some(Void{})` when resolved (void `resolve()` doesn't set `value`).

## ResolveState + PromiseResolver

```cpp
struct ResolveState<T> {
  Option<T>          value;
  AtomicPromiseWaker waker;
  std::atomic<bool>  resolved{false};
};
```

**PromiseResolver::resolve()** — thread-safe, safe after node destruction:

```cpp
void resolve(ValueType &&value) {
    auto s = m_weak.upgrade();               // ArcWeak → Option<Arc<State>>
    if (s.is_some()) {                       // Node still alive?
        auto &state = *s.unwrap();
        if (state.resolved.compare_exchange_strong(false, true, acq_rel)) {
            state.value = Option<T>(std::move(value));
            state.waker.wake();
        }
    }
    // else: node destroyed — silently drop
}
```

## AtomicPromiseWaker — Lock-Free 2-Bit State Machine

Coordinates `register_waker()` (poll side) and `wake()` (resolve side) without a mutex:

| State | Bits | Meaning |
| ------- | ------ | --------- |
| WAITING | `00` | Idle |
| REGISTERING | `01` | poll side storing waker |
| WAKING | `10` | resolve side waking |
| RACE | `11` | Both collided — registerer self-wakes |

```text
register_waker(new_waker):
    CAS(00 → 01)
    ├─ success → store waker → exchange(01 → 00)
    │            ├─ prev == 00 → done
    │            └─ prev == 11 → self-wake
    └─ failure →
         ├─ prev == 10 → wake(new_waker)
         └─ prev == 11 → spin/CAS again

wake():
    fetch_or(10)
    ├─ prev == 00 → exclusive → wake stored waker → store(00)
    └─ prev == 01 → race → set WAKING bit
        └─ registerer's exchange sees 11 → self-wakes
```

Three atomic operations total. No spin loops, no mutex. Memory ordering: `store(release)` on resolve, `load(acquire)` on poll, `fetch_or(acq_rel)` on wake.

## PromiseWaker — Same-Thread vs Cross-Thread

```cpp
void wake() const {
    if (m_loop == xEventLoopCurrent()) {
        *m_done = true;           // Same thread — 2 instructions
    } else {
        xEventLoopPost(m_loop,   // Cross-thread — MPSC enqueue + wake
            [](void *a) { *static_cast<bool *>(a) = true; }, m_done);
    }
}
```

16 bytes, trivially copyable. Same-thread: direct flag set. Cross-thread: `xEventLoopPost` (lock-free MPSC) + `xEventLoopWake`.

## Nested wait() Correctness

`xEventLoopRun` does **not** call `xEventLoopLeave` on return. Enter/Leave is scoped to `WaitScope`:

```cpp
class WaitScope {
public:
    explicit WaitScope(const EventLoop &loop) : m_loop(loop.handle()) {
        if (m_loop) xEventLoopEnter(m_loop);
    }
    ~WaitScope() {
        if (m_loop) xEventLoopLeave();
    }
};
```

Call stack for nested `wait()`:

```text
wait()  (outer)
  poll()  →  None
  xEventLoopRun()  →  timer fires → resolve → then() callback
    .then(fn)  →  fn calls inner_promise.wait()
      wait()  (inner)
        poll()  →  Some(result)  →  return
    fn returns result
  done==true  →  poll  →  Some  →  return
```

Both `xEventLoopRun` calls see the same thread-local loop handle. Neither inner `Run` exit unbinds it.

## Integration Status

| Consumer | Node Type | Reason |
| ---------- | ----------- | -------- |
| `async<T>()` | `ManualResolveNode<T>` | Deferred resolve (ArcWeak, safe after destruction) |
| `Promise::resolve(v)` | `ImmediatePromiseNode` | Immediate completion |
| `Promise<void>::after(ms)` | `AdapterPromiseNode<void, TimerAdapter>` | Timer-based delay |
| `Promise<T>::work(fn)` | `AdapterPromiseNode<T, WorkAdapter<T, F>>` | Thread-pool work |
| `Promise<T>::adapt<Adapter>(args)` | `AdapterPromiseNode<T, Adapter>` | Custom adapter |
| `.then(fn)` | `TransformPromiseNode` / `ChainPromiseNode` | Transform / auto-flatten |
| `yield()` | `YieldPromiseNode` | Chain entry point |
| `all(...)` | `AllTuplePromiseNode` / `AllVoidPromiseNode` | Concurrent — wait for all |
| `race(...)` | `RacePromiseNode` | Concurrent — first wins |
