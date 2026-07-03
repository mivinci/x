## Context

`Promise<T>` wraps `Own<PromiseNode<T>>` and drives it via `poll(waker) → Option<T>`. C++20 coroutines are push-based (suspend → resume). The bridge: `CoroutinePromiseNode<T>` is a `PromiseNode<T>` whose `poll()` drives the coroutine — resume, let it run until `co_await` or `co_return`, poll any awaited promise, and repeat.

## Goals / Non-Goals

**Goals:**
- `Promise<T>` as coroutine return type (no `Task<T>`)
- `co_await Promise<U>` for any `U` inside any `Promise<T>` coroutine
- `co_await` works with all existing Promise sources: `resolve()`, `after()`, `work()`, `async()`, `all()`, `race()`, `adapt()`
- Zero impact on C++17 code (`#if XPP_HAS_COROUTINES`)
- Linear async code replacing `.then()` chains

**Non-Goals:**
- Exception propagation (`unhandled_exception` stores but doesn't propagate yet)
- `co_yield` (not applicable to one-shot Promise)
- Custom coroutine allocators
- Symmetric transfer optimization

## Decisions

### D1: Promise<T> is the coroutine return type, not Task<T>

`Promise<T>::promise_type` is the C++20 coroutine promise. `get_return_object()` creates a `CoroutinePromiseNode<T>`, wraps it in `Own<PromiseNode<T>>`, and returns `Promise<T>`. Users write `Promise<int> foo() { co_return 42; }` — no extra type.

Rationale: avoids introducing a parallel type hierarchy. `Promise<T>` already has `wait()`, `then()`, combinators — all work automatically with coroutine-produced Promises.

### D2: CoroutinePromiseNode<T> owns the coroutine_handle

```
Promise<T> (caller holds)
  └─ Own<PromiseNode<T>> → CoroutinePromiseNode<T>
                              ├─ coroutine_handle<promise_type>
                              ├─ Option<T> result
                              ├─ bool done
                              └─ unique_ptr<AwaitState> await_state
```

`CoroutinePromiseNode`'s destructor calls `handle.destroy()` if `!done`. This is safe because:
- If the coroutine is suspended (co_awaiting), `destroy()` is valid (C++20 spec)
- The `await_state`'s `Own<PromiseNode<U>>` is destroyed first (member order), cancelling the awaited promise
- `final_suspend` is `suspend_always`, so the coroutine frame is always valid until explicitly destroyed

### D3: Type-erased AwaitState for co_await Promise<U>

A `Promise<int>` coroutine can `co_await Promise<string>`. The awaited node has type `PromiseNode<string>`, but `CoroutinePromiseNode<int>` doesn't know `string` at compile time. Solution:

```cpp
struct AwaitState {
    virtual ~AwaitState() = default;
    virtual bool poll(const PromiseWaker&) = 0;  // true = ready
};

template <class U>
struct AwaitStateImpl : AwaitState {
    Own<PromiseNode<U>> node;
    Option<U>* value_ptr;  // points to PromiseAwaiter<U>::value_
    bool poll(const PromiseWaker& w) override;
};
```

`PromiseAwaiter<U>::await_suspend()` extracts the node, creates `AwaitStateImpl<U>`, and stores it in the current `CoroutinePromiseNode` via `promise_type`. The value is written to `PromiseAwaiter<U>::value_` (which lives on the coroutine frame) before `handle.resume()`.

### D4: Lazy start — initial_suspend is suspend_always

The coroutine doesn't run until `poll()` is first called. This matches the existing Promise model — a `Promise<T>` is inert until `wait()` (or `then()` chaining) drives it.

### D5: operator co_await() takes && (rvalue)

`Promise<T>::operator co_await() &&` consumes the Promise. After `co_await promise`, the Promise is moved-from (empty). This prevents dangling references and matches the one-shot semantics of `PromiseNode`.

### D6: promise_type lives inside Promise<T>, conditionally

```cpp
template <class T> class Promise {
    // ... C++17 code ...
#if XPP_HAS_COROUTINES
    struct promise_type { ... };
    PromiseAwaiter<T> operator co_await() &&;
#endif
};
```

The `promise_type` and `CoroutinePromiseNode` implementation are in `promise_coroutine.h`, included at the end of `promise.h` (inside `#if XPP_HAS_COROUTINES`). C++17 users never see coroutine code.

### D7: void handling

`Promise<void>::promise_type` provides both `return_value(Void{})` and `return_void()`. Users can write `co_return;` or `co_return Void{};`. `PromiseAwaiter<void>::await_resume()` returns void.

## Risks / Trade-offs

- **[Coroutine frame allocation]** Each coroutine call heap-allocates a frame. Fewer allocations than equivalent `.then()` chains (one frame vs N TransformPromiseNodes), but still one per call. Custom allocators deferred.
- **[No exception propagation]** `unhandled_exception()` stores `std::current_exception()` but there's no `ExceptionOr<T>` to propagate it through the Promise chain. For now, the exception is rethrown in `wait()`. Future: integrate with error/reject support.
- **[Coroutine frame lifetime]** If `Promise<T>` is destroyed before the coroutine completes, `handle.destroy()` runs. The awaited promise's node is destroyed (cancelling it). This is safe but abrupt — no graceful cancellation callback.
- **[C++20 compiler requirement]** Coroutine tests need a C++20-capable compiler. CI must have a separate C++20 build lane or skip coroutine tests on older compilers.
