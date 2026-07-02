## 1. Implement `TimerPromiseNode` class

- [x] 1.1 Add `#include <x/base/event.h>` to `libxpp/xpp/promise_node.h` (if not already present — it's likely transitively included via `xpp/event.h`)
- [x] 1.2 Add `#include <atomic>` to `libxpp/xpp/promise_node.h` (already present via `promise_waker.h`, but verify)
- [x] 1.3 Add `class TimerPromiseNode final : public PromiseNode<void>` in `libxpp/xpp/promise_node.h`, placed after `YieldPromiseNode` and before the closing `}  // namespace _`. Members:
  - `xTimer m_handle` (raw handle; dangling after fire)
  - `std::atomic<bool> m_fired`
  - `PromiseAtomicWaker m_waker`
  - Constructor: `explicit TimerPromiseNode(uint64_t ms)` — calls `xTimerStart(fire_cb, this, on_cancel_cb, ms, 0)`, stores handle, inits `m_fired=false`
  - `~TimerPromiseNode()` override: if `!m_fired.load(acquire)`, call `xTimerStop(m_handle)`. Otherwise skip (handle is dangling or null).
  - `Option<Void> poll(const PromiseWaker &waker) override`: if `m_fired.load(acquire)`, return `Some(Void)`; else register waker, re-check, return `Some` or `None`
  - `static void fire_cb(void *arg)`: cast to `TimerPromiseNode*`, `m_fired.store(true, release)`, `m_waker.wake()`
  - `static void on_cancel_cb(void *arg)`: cast to `TimerPromiseNode*`, `m_handle = nullptr`, `m_fired.store(true, release)`, `m_waker.wake()`
- [x] 1.4 Add docstring on `TimerPromiseNode` documenting: (a) the single-thread destruction contract, (b) why `m_fired` gates `xTimerStop`, (c) that `on_cancel_cb` is invoked by libx on loop destroy

## 2. Rewrite `Promise<void>::after(ms)`

- [x] 2.1 Replace the body of `Promise<T>::after(uint64_t ms)` in `libxpp/xpp/promise.h` with: `return Promise<void>(Own<_::PromiseNode<void>>(new _::TimerPromiseNode(ms)));`
- [x] 2.2 Remove any now-unused includes from `promise.h` if the `PromiseResolver<void>` forward-declaration and the lambda dance were the only users (likely none — `PromiseResolver` is still used elsewhere)
- [x] 2.3 Update the docstring of `Promise<void>::after(ms)` to note that the promise owns a `TimerPromiseNode` and must be destroyed on the WaitScope thread

## 3. Tests — new coverage for previously-broken scenarios

- [x] 3.1 Add `Promise.AfterDestroyedBeforeFire` test in `libxpp/xpp/promise_test.cpp`: construct `Promise<void>::after(1000)`, immediately drop the promise (don't call `wait()`), run the loop briefly to confirm no crash. Use ASan if available.
- [x] 3.2 Add `Promise.AfterLoopDestroyedBeforeFire` test: construct `Promise<void>::after(1000)`, destroy the event loop without firing, then destroy the promise. Assert no crash and no leak (verify with ASan if available).
- [x] 3.3 Add `Promise.AfterStillResolvesOnFire` regression test: the existing happy path — `after(10).wait()` returns after ~10ms. Ensure this still works.
- [~] 3.4 Add `Promise.AfterPollAfterFire` test: poll the promise after it has fired, assert it returns `Some(Void)` without crashing (regression for "fire_cb sets m_fired before wake" ordering). **SKIPPED**: `poll()` is internal to `PromiseNode` and not exposed via `Promise<void>::after`'s public API. The `AfterStillResolvesOnFire` test covers the same semantic through the public API (wait() returning means poll() returned Some).
- [x] 3.5 Add `Promise.AfterThenChain` test: `after(10).then([]() { return 42; }).wait()` returns 42. Ensures composition with `then` still works after the rewrite.
- [x] 3.6 Run existing `promise_test.cpp` and `promise_deadlock_test.cpp` — all existing tests must pass unchanged.

## 4. Docs

- [x] 4.1 Update `docs/libxpp/promise.md` — replace the implementation note in the "Integration status" table (or equivalent) to reflect that `after()` is now backed by `TimerPromiseNode` and is safe to drop before fire
- [x] 4.2 Add a short note in `docs/libxpp/promise.md` (in the `after()` section) that the returned promise must be destroyed on the WaitScope thread
- [x] 4.3 Rebuild mdBook: `cd docs && mdbook build`
- [x] 4.4 Verify the new docs appear in `book/libxpp/promise.html`

## 5. Build & local verification

- [x] 5.1 Configure and build on macOS: `cmake -B build -G Ninja -DX_TLS_BACKEND=openssl && cmake --build build -j`
- [x] 5.2 Run all libxpp tests: `cd build && ctest -R "promise|option|own|box|nonnull|panic|event" --output-on-failure`
- [x] 5.3 Run the new tests specifically: `./build/libxpp/xpp/promise_test --gtest_filter="Promise.After*"`
- [x] 5.4 Run full ctest: `cd build && ctest --output-on-failure` — all 18 executables must pass

## 6. Code quality

- [x] 6.1 Run `find libxpp \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print0 | xargs -0 clang-format --dry-run --Werror` — must pass
- [x] 6.2 Run `./scripts/run-clang-tidy.sh -j $(sysctl -n hw.ncpu)` — verify no new `google-readability-casting` violations in the new `TimerPromiseNode` code

## 7. ASan verification (if available)

- [~] 7.1 Attempt `zsh scripts/test-mac.sh -t openssl -j $(sysctl -n hw.ncpu) --asan`. If the local ASan init deadlock (macOS 26.5 tooling issue) recurs, document and defer to Linux CI. **SKIPPED**: ASan init deadlock on macOS 26.5 (same issue as x-timer-on-cancel PR). Deferred to Linux CI on push.
- [~] 7.2 If ASan runs: verify the two new tests (3.1, 3.2) report zero leaks and zero UAF errors. **SKIPPED**: see 7.1

## 8. Pre-PR

- [ ] 8.1 Commit all changes on a feature branch `feat/xpp-timer-promise-node`
- [ ] 8.2 Push branch and create PR targeting `main`. PR body should reference the openspec change `xpp-timer-promise-node` and call out that this is the first consumer of the `on_cancel` hook added in PR #7.
