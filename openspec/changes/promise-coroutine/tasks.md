## 1. CoroutinePromiseNode<T>

- [ ] 1.1 Create `promise_coroutine.h` with `#if XPP_HAS_COROUTINES` guard. Include `<coroutine>`, `promise.h`, `promise_adapter.h`.
- [ ] 1.2 Define `AwaitState` base (virtual `~AwaitState`, virtual `bool poll(const PromiseWaker&)`)
- [ ] 1.3 Define `AwaitStateImpl<U>` — holds `Own<PromiseNode<U>>` + `Option<U>* value_ptr`. `poll()` calls `node->poll(waker)`, stores result in `*value_ptr` if ready.
- [ ] 1.4 Define `CoroutinePromiseNode<T>` — holds `coroutine_handle`, `Option<T> result`, `bool done`, `unique_ptr<AwaitState> await_state`. Implements `PromiseNode<T>::poll()`.
- [ ] 1.5 Implement `poll()`: first call → `handle.resume()`; has await_state → poll it, resume if ready; done → return `Some(result)`.
- [ ] 1.6 Implement `set_await<U>(Own<PromiseNode<U>>, Option<U>*)` — creates `AwaitStateImpl<U>`, stores in `await_state`.
- [ ] 1.7 Implement destructor: `if (handle_ && !done_) handle_.destroy();`

## 2. Promise<T>::promise_type

- [ ] 2.1 Define `promise_type` inside `Promise<T>` (behind `#if XPP_HAS_COROUTINES`). Holds `CoroutinePromiseNode<T>* node`.
- [ ] 2.2 `get_return_object()` — create `CoroutinePromiseNode`, set `handle = from_promise(*this)`, return `Promise<T>(Own<PromiseNode<T>>(node))`.
- [ ] 2.3 `initial_suspend()` → `std::suspend_always` (lazy start)
- [ ] 2.4 `final_suspend()` noexcept → `std::suspend_always` (don't auto-destroy)
- [ ] 2.5 `return_value(ValueType v)` — store in `node->result`, set `node->done = true`
- [ ] 2.6 `return_void()` — for `Promise<void>`, store `Void{}`, set done
- [ ] 2.7 `unhandled_exception()` — store `std::current_exception()` in node for later rethrow
- [ ] 2.8 `get_return_object_on_allocation_failure()` → return `Promise<T>()` (empty)

## 3. PromiseAwaiter<T>

- [ ] 3.1 Define `PromiseAwaiter<T>` — holds moved `Promise<T>` + `Option<T> value_`
- [ ] 3.2 `await_ready()` → `false`
- [ ] 3.3 `await_suspend(handle)` — extract node via `_extract_node`, call `handle.promise().node->set_await(std::move(node), &value_)`
- [ ] 3.4 `await_resume()` → `return std::move(value_).unwrap()`
- [ ] 3.5 `set_value(T&&)` — `value_ = std::move(v)`
- [ ] 3.6 Void specialization `PromiseAwaiter<void>` — `await_resume()` returns void, uses `bool ready_` instead of `Option<Void>`

## 4. Promise<T>::operator co_await

- [ ] 4.1 Add `PromiseAwaiter<T> operator co_await() &&` to `Promise<T>` (behind `#if XPP_HAS_COROUTINES`)
- [ ] 4.2 Add void specialization: `PromiseAwaiter<void> operator co_await() &&` on `Promise<void>`

## 5. Integration in promise.h

- [ ] 5.1 Add conditional `#include <xpp/promise_coroutine.h>` at the end of `promise.h` (inside `#if XPP_HAS_COROUTINES`)
- [ ] 5.2 Add `#if XPP_HAS_COROUTINES` forward declarations for `CoroutinePromiseNode`, `PromiseAwaiter`

## 6. CMake

- [ ] 6.1 Add `promise_coroutine_test.cpp` to CMakeLists.txt GLOB (auto-discovered)
- [ ] 6.2 Set C++20 compile feature on the coroutine test target
- [ ] 6.3 Skip coroutine test target if compiler doesn't support C++20 coroutines

## 7. Tests

- [ ] 7.1 Simple coroutine: `Promise<int> foo() { co_return 42; }` → `wait() == 42`
- [ ] 7.2 Coroutine with co_await resolve: `co_await Promise<int>::resolve(10)` → linear chain
- [ ] 7.3 Coroutine with co_await after: `co_await Promise<void>::after(50)` → delay
- [ ] 7.4 Coroutine with co_await work: `co_await Promise<int>::work(fn)` → thread pool
- [ ] 7.5 Coroutine with co_await async: `co_await` a deferred promise from `async()`
- [ ] 7.6 Coroutine returning void: `Promise<void> foo() { co_return; }` → `wait()`
- [ ] 7.7 Nested coroutines: coroutine A co_awaits coroutine B
- [ ] 7.8 Coroutine with all(): `co_await xpp::all(p1, p2)` → tuple result
- [ ] 7.9 Coroutine with race(): `co_await xpp::race(p1, p2)` → first result
- [ ] 7.10 Coroutine with then(): `coroutine().then(fn).wait()` — coroutine result feeds into then chain
- [ ] 7.11 Early destruction: create coroutine Promise, destroy without wait → no crash
- [ ] 7.12 Multiple sequential co_awaits: `a = co_await f1(); b = co_await f2(); co_return a + b;`

## 8. Docs

- [ ] 8.1 Create `docs/libxpp/promise/coroutine.md` — coroutine usage guide
- [ ] 8.2 Update `docs/libxpp/promise/README.md` — add coroutine to Topics + API table
- [ ] 8.3 Update `docs/SUMMARY.md` — add coroutine page
