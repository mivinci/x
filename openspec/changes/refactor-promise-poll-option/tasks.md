## 1. Rewrite promise_node.h

- [x] 1.1 Remove `SpawnTaskBase`, `Schedule`, `SyncWaitSchedule`, `CoroWakeSchedule`, and all `#if XPP_HAS_COROUTINES` blocks
- [x] 1.2 Simplify `Waker` to `{void(*fn)(void*), void*arg}` with `wake()` and `sync_wait()` static factory
- [x] 1.3 Change `PromiseNode<T>::poll` signature to `virtual Option<ValueType> poll(Waker waker) = 0`
- [x] 1.4 Remove `PromiseNode<T>::take()` — no longer exists
- [x] 1.5 Rewrite `ImmediatePromiseNode<T>` — `poll` returns `Some(value)` directly
- [x] 1.6 Rewrite `TransformPromiseNode<U, T, Func>` — `poll` upstream, `Some` → apply func → `Some(result)`, `None` → `None`. Keep all 4 void/non-void specializations.
- [x] 1.7 Rewrite `ChainPromiseNode<T>` — use `m_inner != nullptr` instead of `Step1`/`Step2` enum
- [x] 1.8 Rewrite `AdapterPromiseNode<T>` — `poll` returns `Option<ValueType>`, `resolve` stores value + fires waker. Retain `AtomicWaker`.
- [x] 1.9 Rewrite `YieldPromiseNode` — `poll` returns `Some(Void{})`
- [x] 1.10 Remove `extern "C" { #include <x/base/event.h> }` — use `#include <xpp/event.h>` instead
- [x] 1.11 Verify `promise_node.h` compiles standalone (no missing includes)

## 2. Rewrite promise.h

- [x] 2.1 Remove all `#if XPP_HAS_COROUTINES` blocks (`promise_type`, `operator co_await`, `#include <coroutine>`)
- [x] 2.2 Remove `release_node()` (internal, no longer needed without runtime)
- [x] 2.3 Rewrite `wait()` to use local `bool done` flag + `Waker::sync_wait()` + poll loop returning `Option`
- [x] 2.4 Update `then()` implementations to use new `TransformPromiseNode` / `ChainPromiseNode` (poll-only, no take)
- [x] 2.5 Update `Promise::make()` — `AdapterPromiseNode` interface changed (no take)
- [x] 2.6 Update `Promise::resolve()` — `ImmediatePromiseNode` poll returns `Some`
- [x] 2.7 Update `Promise::eval()` — `YieldPromiseNode` poll returns `Some`
- [x] 2.8 Update `Resolver<T>` — `AdapterPromiseNode::resolve()` signature unchanged (still takes value), but internal storage uses `Option`
- [x] 2.9 Remove `#include <xpp/result.h>` if unused after coroutine removal
- [x] 2.10 Verify `promise.h` compiles standalone

## 3. Write promise tests

- [x] 3.1 `Promise<int>::resolve(42).wait()` returns 42 (immediate, no loop run)
- [x] 3.2 `Promise<void>::resolve().wait()` completes (void immediate)
- [x] 3.3 `Promise<int>::resolve(1).then([](int x){ return x + 1; }).wait()` returns 2 (transform chain)
- [x] 3.4 `Promise::make()` + timer resolve + `wait()` (deferred, loop runs)
- [x] 3.5 `Promise::make()` + `then()` + `wait()` (deferred transform)
- [x] 3.6 `Promise<void>::make()` + resolve + `wait()` (void deferred)
- [x] 3.7 `then()` returning `Promise<U>` flattens correctly (ChainPromiseNode)
- [x] 3.8 `Promise<void>::eval([] { return 42; }).wait()` returns 42
- [x] 3.9 `yield().then([] { return 1; }).wait()` returns 1
- [x] 3.10 All tests run inside `WaitScope`

## 4. Verify

- [x] 4.1 `cmake --build build -j` — clean build
- [x] 4.2 `ctest -R "promise_test" --output-on-failure` — all tests pass
- [x] 4.3 `ctest -R "event_test|box_test|own_test|option_test|nonnull_test|panic_test"` — no regression
