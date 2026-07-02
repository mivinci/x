## 1. Implement `xpp::Timer` class

- [x] 1.1 Create `libxpp/xpp/timer.h` with the `Timer` class:
  - `struct State` (base): `uint64_t timeout_ms`, `uint64_t repeat_ms`, `xTimer handle`, `virtual void invoke() = 0`, `virtual ~State() = default`
  - `template <class F> struct StateImpl : State`: stores `F f`, overrides `invoke()` to call `f()`
  - `Own<State> m_state` member
  - Two templated constructors: `Timer(uint64_t ms, F&& cb)` and `Timer(uint64_t timeout_ms, uint64_t repeat_ms, F&& cb)`
  - Move ctor/assignment (defaulted, transfers `Own<State>`)
  - Deleted copy ctor/assignment
  - `~Timer()` calls `stop()` if `m_state && m_state->handle`
  - `void stop()` — if `m_state && m_state->handle`: `xTimerStop(handle); m_state->handle = nullptr`
  - `bool start()` — if `!m_state || m_state->handle` return false; call `xTimerStart(&Timer::fire, m_state.get(), &Timer::cancel, m_state->timeout_ms, m_state->repeat_ms)`; store result in `m_state->handle`; return `handle != nullptr`
  - `bool is_active() const noexcept` — `m_state && m_state->handle != nullptr`
  - `explicit operator bool() const noexcept` — `is_active()`
  - `xTimer handle() const noexcept` — `m_state ? m_state->handle : nullptr`
  - `static void fire(void *arg)` — `static_cast<State*>(arg)->invoke()`; if `repeat_ms == 0` set `handle = nullptr`
  - `static void cancel(void *arg)` — `static_cast<State*>(arg)->handle = nullptr`
- [x] 1.2 Add `#include <xpp/own.h>` and `#include <xpp/event.h>` to `timer.h`
- [x] 1.3 Add docstring on the class covering: WaitScope contract, one-shot vs repeating, pause/resume semantics, callback exception policy, self-stop from callback
- [x] 1.4 Add `libxpp/xpp/timer.h` to `libxpp/xpp/CMakeLists.txt` header list

## 2. Tests

- [x] 2.1 Create `libxpp/xpp/timer_test.cpp` with the following tests:
  - `Timer.RepeatingFiresMultipleTimes` — construct `Timer(50, cb)`, run loop, assert callback fired ≥3 times
  - `Timer.OneShotFiresOnce` — construct `Timer(50, 0, cb)`, run loop, assert callback fired exactly once and `is_active()` is false after
  - `Timer.AsymmetricRepeating` — construct `Timer(10, 100, cb)`, verify first fire <30ms and second fire ~100ms after first
  - `Timer.StopCancelsPending` — construct repeating timer, call `stop()`, run loop, assert callback never fires
  - `Timer.StopIsIdempotent` — call `stop()` twice, no crash
  - `Timer.StartResumesAfterStop` — construct, stop, start again, verify callback fires again
  - `Timer.StartWhenAlreadyActiveReturnsFalse` — construct, call `start()`, assert returns false
  - `Timer.SelfStopFromCallback` — callback calls `stop()` on its own timer after N ticks, verify timer stops
  - `Timer.MoveTransfersOwnership` — move-construct, verify source is inactive and destination fires
  - `Timer.MoveAssignStopsPrevious` — move-assign into an active timer, verify previous timer is stopped
  - `Timer.DestructorStopsActive` — construct in a scope, exit scope, verify callback stops firing
  - `Timer.LoopDestroyedBeforeFire` — construct a long-delay timer, destroy loop, then destroy timer; no crash
  - `Timer.CallbackOutsideWaitScope` — construct outside WaitScope, assert `is_active()` is false and callback never fires
  - `Timer.HandleReturnsValidXTimer` — while active, `handle()` returns non-null; after stop, returns null
- [x] 2.2 Add `timer_test.cpp` to `libxpp/xpp/CMakeLists.txt` test sources and register with CTest as `timer_test`
- [x] 2.3 Build and run: `cmake --build build -j --target timer_test && ./build/libxpp/xpp/timer_test`

## 3. Docs

- [x] 3.1 Create `docs/libxpp/timer.md` covering:
  - Introduction: callback-style timer, one-shot and repeating, RAII
  - API reference table (constructors, `stop`, `start`, `is_active`, `handle`)
  - Usage examples: repeating, one-shot, asymmetric, pause/resume, self-stop
  - Comparison with `Promise<void>::after(ms)` — when to use which
  - Notes: callback exception policy, no deadline preservation on resume
- [x] 3.2 Add link to `docs/SUMMARY.md` for the timer page (after the promise page)
- [x] 3.3 Rebuild mdBook: `cd docs && mdbook build`
- [x] 3.4 Verify the new page appears in `book/libxpp/timer.html`

## 4. Build & local verification

- [x] 4.1 Configure and build: `cmake -B build -G Ninja && cmake --build build -j`
- [x] 4.2 Run full ctest: `cd build && ctest --output-on-failure` — all executables must pass (existing + new `timer_test`)
- [x] 4.3 Run timer tests specifically: `./build/libxpp/xpp/timer_test`

## 5. Code quality

- [x] 5.1 Run `find libxpp \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print0 | xargs -0 clang-format --dry-run --Werror` — must pass
- [x] 5.2 Run `./scripts/run-clang-tidy.sh -j $(sysctl -n hw.ncpu)` — verify no new violations in `timer.h` or `timer_test.cpp`

## 6. Pre-PR

- [x] 6.1 Commit all changes on a feature branch `feat/xpp-add-timer`
- [ ] 6.2 Push branch and create PR targeting `main`. PR body should reference the openspec change `xpp-add-timer` and note that `Timer` is the second consumer of the `on_cancel` hook (after `TimerPromiseNode`).
