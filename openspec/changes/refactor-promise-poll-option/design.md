## Context

`libxpp/xpp/promise_node.h` and `promise.h` were ported from the `libx++` project, which has a full async runtime (Scheduler, BlockingPool, SpawnTaskBase). libxpp doesn't have that runtime — it relies on `EventLoop` + `WaitScope` only. The ported code carries dead infrastructure (~100 lines of `SpawnTaskBase`/`Schedule`), a two-step `poll` + `take` protocol with implicit state, and a `wait()` method that references an undeclared `m_done` member.

## Goals / Non-Goals

**Goals:**
- `PromiseNode::poll` returns `Option<ValueType>` — readiness and value in one call
- Eliminate `take()` entirely
- Remove `SpawnTaskBase`, `Schedule`, `SyncWaitSchedule`, `CoroWakeSchedule`
- Simplify `Waker` to a function-pointer + arg (no virtual dispatch)
- Fix `wait()` to use a local flag
- Simplify `ChainPromiseNode` (no enum state machine)
- Retain `AtomicWaker` for thread-safe `AdapterPromiseNode`
- Keep node naming: `ImmediatePromiseNode`, `TransformPromiseNode`, etc.

**Non-Goals:**
- C++20 coroutine support (`co_await`) — deferred to a future change
- Multi-threaded task spawning (`spawn()`/`join()`) — deferred
- Changes to `EventLoop`, `WaitScope`, `Own`, `Box`, `Option`, or other infrastructure

## Decisions

### D1: poll returns Option<T> instead of bool + take()

**Choice:** `virtual Option<ValueType> poll(Waker waker) = 0;`

`Some(value)` = ready, `None` = pending (waker stored). No `take()`.

**Rationale:** Eliminates the implicit "poll returns true → take is now valid" contract. Value and readiness are atomic — one call. `Option<T>` already has niche optimization (`sizeof == sizeof(T*)`), so zero overhead vs `bool`. `Option<Void>` handles `Promise<void>` cleanly.

### D2: Waker simplified to function pointer + arg

**Choice:**
```cpp
class Waker {
  void (*m_fn)(void *);
  void  *m_arg;
public:
  void wake() const { if (m_fn) m_fn(m_arg); }
  static Waker sync_wait(bool *done, xEventLoop loop);
};
```

**Rationale:** The `Schedule`/`SpawnTaskBase` virtual hierarchy was designed for runtime-managed task scheduling. libxpp's only use case is "post a flag-set to the event loop" — a single function pointer suffices. Saves ~100 lines of dead code.

### D3: ChainPromiseNode uses null-check instead of enum

**Choice:** `m_inner` starts as `nullptr`; when `m_outer->poll` returns `Some(Promise<T>)`, extract the inner node and poll it.

**Rationale:** `m_inner != nullptr` is equivalent to "we've transitioned from Step1 to Step2" but without the enum and `XPP_UNREACHABLE()` default case.

### D4: AtomicWaker retained

**Choice:** Keep `AtomicWaker` as-is (lock-free 2-bit state machine).

**Rationale:** `AdapterPromiseNode` (used by `Promise::make()`) needs concurrent `poll` (register waker) and `resolve` (fire waker) to be safe. `AtomicWaker` handles the race without a mutex. Even though libxpp is single-threaded within a `WaitScope`, `Resolver::resolve()` may be called from another thread (e.g., a callback on a thread pool).

### D5: Coroutine support removed

**Choice:** Remove all `#if XPP_HAS_COROUTINES` blocks.

**Rationale:** libxpp targets C++17, not C++20. Coroutine support (`co_await`, `promise_type`) can be added in a future change when libxpp moves to C++20 or when `XPP_HAS_COROUTINES` is explicitly enabled. Keeping it now adds complexity and untested code paths.

## Risks / Trade-offs

- **[Option<T> nesting]** If `T = Option<U>`, `poll` returns `Option<Option<U>>`. This is correct (`None` = pending, `Some(None)` = ready with no value) but slightly confusing. Acceptable — `Promise<Option<U>>` is rare, and the semantics are unambiguous.

- **[Waker function pointer is not type-safe]** The `void*` arg erases the type. The static factory `Waker::sync_wait()` ensures the fn and arg match. User-constructed custom wakers must be careful. Acceptable — same pattern as libx's C callback API.

- **[No coroutine support]** Users who want `co_await` must wait for a future change. Not blocking — `then()` chains cover the same use cases, just with more nesting.

- **[Breaking change for libx++ users]** The libx++ project (separate repo) uses the old `poll`+`take` interface. libxpp is a fresh copy, so no external breakage — but the two will diverge. Acceptable — they're separate projects.
