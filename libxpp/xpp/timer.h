/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer.h - xpp::Timer: callback-style timer with pause/resume.
 *
 * Supports both one-shot (repeat_ms == 0) and repeating
 * (repeat_ms > 0) timers via a callback API. Complements
 * Promise<void>::after(ms) which is Promise-based and one-shot only.
 *
 * Must be constructed within a WaitScope. The callback runs on the
 * WaitScope thread. Move-only; RAII destructor stops the timer.
 *
 * Uses xTimerStart's on_cancel hook (added in the x-timer-on-cancel
 * change) for safe shutdown when the host loop is destroyed.
 *
 * C++17-compatible. Header-only.
 */

#ifndef XPP_TIMER_H
#define XPP_TIMER_H

#include <cstdint>

#include <xpp/event.h>
#include <xpp/own.h>

#include <x/base/event.h>

namespace xpp {

/**
 * @brief Callback-style timer with pause/resume.
 *
 * Wraps `xTimerStart` / `xTimerStop` in a move-only RAII class.
 * Supports both one-shot (`repeat_ms == 0`) and repeating
 * (`repeat_ms > 0`) timers.
 *
 * @par Construction
 * Two overloads:
 * - `Timer(ms, cb)` — symmetric repeating (timeout = repeat = ms)
 * - `Timer(timeout_ms, repeat_ms, cb)` — explicit; `repeat_ms == 0`
 *   for one-shot
 *
 * @par WaitScope contract
 * Must be constructed within a WaitScope. The callback runs on the
 * WaitScope thread. `start()` after `stop()` also requires a live
 * WaitScope on the current thread.
 *
 * @par Pause / resume
 * `stop()` cancels the current timer but keeps `timeout_ms` and
 * `repeat_ms` so `start()` can reschedule. `start()` does NOT
 * preserve the original deadline — the next fire is `timeout_ms`
 * away, matching libuv's `uv_timer_stop` / `uv_timer_start` semantics.
 *
 * @par Callback exceptions
 * If the callback throws, the exception propagates through
 * `xEventLoopRun` to the caller of `loop.run()`. This matches libuv
 * behavior. Throwing callbacks are the user's responsibility.
 *
 * @par Self-stop from callback
 * Calling `stop()` from inside the callback is safe and supported.
 * libx re-arms repeating timers before invoking the callback, so
 * `xTimerStop` will find a valid timer in the heap and remove it.
 *
 * @par Loop-destroy cleanup
 * When the host event loop is destroyed with the timer still
 * pending, libx invokes the `on_cancel` hook, which nulls the
 * stored handle. The destructor then sees a null handle and skips
 * `xTimerStop`. The user callback is NOT invoked on this path.
 *
 * @par Comparison with Promise<void>::after(ms)
 * - `after(ms)` is Promise-based, one-shot, integrates with `.then()`
 * - `Timer` is callback-based, supports repeating, integrates with
 *   nothing (just fires the callback)
 *
 * Use `after(ms)` when you need Promise composition. Use `Timer`
 * when you need a periodic callback or a simple one-shot callback
 * without Promise overhead.
 */
class Timer {
public:
  /// @brief Symmetric repeating: fires every @p ms milliseconds.
  ///
  /// Equivalent to `Timer(ms, ms, cb)`.
  template <class F> Timer(uint64_t ms, F &&cb) : Timer(ms, ms, std::forward<F>(cb)) {}

  /// @brief Explicit construction with separate timeout and repeat.
  ///
  /// @param timeout_ms  Delay before the first fire.
  /// @param repeat_ms   Interval for subsequent fires. 0 = one-shot.
  /// @param cb          Callback invoked on each fire.
  template <class F>
  Timer(uint64_t timeout_ms, uint64_t repeat_ms, F &&cb)
      : m_state(make_state(timeout_ms, repeat_ms, std::forward<F>(cb))) {
    m_state->handle =
      xTimerStart(&Timer::fire, m_state.get(), &Timer::cancel, timeout_ms, repeat_ms);
  }

  ~Timer() {
    if (m_state && m_state->handle) {
      stop();
    }
  }

  Timer(Timer &&) noexcept = default;
  Timer &operator=(Timer &&o) noexcept {
    if (this != &o) {
      stop(); // release current timer before overwriting m_state
      m_state = std::move(o.m_state);
    }
    return *this;
  }
  Timer(const Timer &)            = delete;
  Timer &operator=(const Timer &) = delete;

  /// @brief Stop the timer. Idempotent.
  ///
  /// After `stop()`, the callback will not fire again. `start()` can
  /// reschedule the timer with the original `timeout_ms` / `repeat_ms`.
  void stop() {
    if (m_state && m_state->handle) {
      xTimerStop(m_state->handle);
      m_state->handle = nullptr;
    }
  }

  /// @brief Resume the timer after `stop()`.
  ///
  /// Schedules a new `xTimerStart` with the original parameters.
  /// Returns `false` if the timer is already active or no live event
  /// loop is registered on the current thread.
  ///
  /// Does NOT preserve the original deadline — the next fire is
  /// `timeout_ms` away.
  bool start() {
    if (!m_state || m_state->handle) return false;
    m_state->handle = xTimerStart(&Timer::fire, m_state.get(), &Timer::cancel, m_state->timeout_ms,
                                  m_state->repeat_ms);
    return m_state->handle != nullptr;
  }

  /// @brief True if the timer is currently scheduled (has a non-null handle).
  bool is_active() const noexcept {
    return m_state && m_state->handle != nullptr;
  }

  /// @brief Equivalent to `is_active()`.
  explicit operator bool() const noexcept {
    return is_active();
  }

  /// @brief Underlying `xTimer` handle for C API interop, or nullptr.
  xTimer handle() const noexcept {
    return m_state ? m_state->handle : nullptr;
  }

private:
  struct State {
    uint64_t     timeout_ms;
    uint64_t     repeat_ms;
    xTimer       handle;
    virtual void invoke() = 0;
    virtual ~State()      = default;
  };

  template <class F> struct StateImpl : State {
    F f;
    StateImpl(uint64_t timeout_ms, uint64_t repeat_ms, F &&fn) : f(std::move(fn)) {
      this->timeout_ms = timeout_ms;
      this->repeat_ms  = repeat_ms;
      this->handle     = nullptr;
    }
    void invoke() override {
      f();
    }
  };

  Own<State> m_state;

  template <class F> static Own<State> make_state(uint64_t timeout_ms, uint64_t repeat_ms, F &&cb) {
    using DecayedF = typename std::decay<F>::type;
    return Own<State>(new StateImpl<DecayedF>(timeout_ms, repeat_ms, std::forward<F>(cb)));
  }

  static void fire(void *arg) {
    auto *s = static_cast<State *>(arg);
    s->invoke();
    if (s->repeat_ms == 0) {
      // One-shot: libx has already timer_free'd the struct; handle is
      // dangling. Null it so ~Timer / stop() skip xTimerStop.
      s->handle = nullptr;
    }
    // Repeating: libx re-armed before calling us; handle still valid.
  }

  static void cancel(void *arg) {
    auto *s = static_cast<State *>(arg);
    // libx has recycled the timer struct during loop destroy.
    s->handle = nullptr;
  }
};

} // namespace xpp

#endif // XPP_TIMER_H
