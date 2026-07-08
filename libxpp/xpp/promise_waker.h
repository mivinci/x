/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_waker.h - PromiseWaker + AtomicPromiseWaker.
 *
 * PromiseWaker captures an event loop handle + done flag.  When fired,
 * it sets done = true and (in the fiber path) switches back to the
 * waiting fiber.
 *
 * The constructor requires a live WaitScope — it asserts m_loop != NULL.
 * There is no inert/null-waker state.  AtomicPromiseWaker uses
 * Option<PromiseWaker> to model "waker not yet registered".
 *
 * The implementation is split in two at XPP_FIBER:
 *
 *   !XPP_FIBER — same-thread sets the flag directly, cross-thread posts
 *                 on_done.  park() runs X_RUN_ONCE in a loop.
 *
 *   XPP_FIBER  — adds fiber-aware wake/park: same-thread wake does a
 *                 direct xFiberSwitch; park() calls xFiberYield() instead
 *                 of running the event loop.
 *
 * The done flag is a pointer to m_storage rather than an inline bool.
 * This is necessary because PromiseWaker is copyable, and copies must
 * share the same flag — AtomicPromiseWaker stores a copy, and its wake()
 * must visibly set the flag that wait()'s park() checks.
 *
 * sizeof(PromiseWaker): 24B (32B with XPP_FIBER).
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_WAKER_H
#define XPP_PROMISE_WAKER_H

#include <atomic>

#include <xpp/event.h>
#include <xpp/option.h>

#if XPP_FIBER
#include <x/base/fiber.h>
#endif

namespace xpp {

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — declaration
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Waker — captures event loop + done flag + optional fiber.
 *
 * Must be constructed inside a WaitScope.  The event loop handle is
 * asserted non-null — there is no inert-waker state.
 *
 * Implementation is split at XPP_FIBER:
 *   - Non-fiber: same-thread sets flag directly, cross-thread posts
 *     on_done.  park() runs xEventLoopRun(X_RUN_ONCE) in a loop.
 *   - Fiber: same-thread does a direct xFiberSwitch to resume the
 *     waiting fiber.  park() calls xFiberYield() instead.
 */
class PromiseWaker {
public:
  /**
   * @brief Construct a waker bound to the current event loop.
   *
   * The event loop handle is captured from xEventLoopCurrent().
   * Must be inside a WaitScope — absence is a fatal assert.
   *
   * When XPP_FIBER is enabled, also captures the current fiber
   * handle via xFiberCurrent() for cooperative parking.
   */
  PromiseWaker();

  /**
   * @brief Notify the waiter that the promise may be ready.
   *
   * Same-thread: sets the done flag, then (if fiber) does a direct
   * xFiberSwitch to resume the waiting fiber.
   *
   * Cross-thread: posts set-done + optional fiber-switch to the
   * event loop's done queue.
   */
  void wake() const;

  /**
   * @brief Park the current context until wake() is called, then reset.
   *
   * Non-fiber: runs xEventLoopRun(X_RUN_ONCE) in a busy-wait loop.
   * Fiber:     calls xFiberYield() to suspend and yield the CPU
   *            back to the event loop.
   */
  void park() const;

private:
  xEventLoop m_loop;
  bool      *m_done;
  bool       m_storage = false;
#if XPP_FIBER
  xFiber m_fiber = nullptr;

  /// Cross-thread: sets the done flag + switches to the fiber.
  static void on_fiber_cross_thread_wake(void *arg);
#endif

  /// Cross-thread done callback (non-fiber path).
  static void on_done(void *arg);
};

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — implementation (non-fiber)
 * ═══════════════════════════════════════════════════════════════════════ */

#if !XPP_FIBER

inline PromiseWaker::PromiseWaker() : m_done(&m_storage) {
  m_loop = xEventLoopCurrent();
  XPP_ASSERT(m_loop, "PromiseWaker requires a WaitScope");
}

inline void PromiseWaker::wake() const {
  if (m_loop == xEventLoopCurrent()) {
    *m_done = true;
  } else {
    xEventLoopPost(m_loop, &on_done,
                   const_cast<void *>(static_cast<const void *>(this)));
  }
}

inline void PromiseWaker::park() const {
  while (!*m_done) {
    xEventLoopRun(m_loop, X_RUN_ONCE);
  }
  *m_done = false;
}

inline void PromiseWaker::on_done(void *arg) {
  *static_cast<PromiseWaker *>(arg)->m_done = true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — implementation (fiber)
 * ═══════════════════════════════════════════════════════════════════════ */

#else // XPP_FIBER

inline PromiseWaker::PromiseWaker() : m_done(&m_storage) {
  m_loop  = xEventLoopCurrent();
  m_fiber = xFiberCurrent();
  XPP_ASSERT(m_loop, "PromiseWaker requires a WaitScope");
}

inline void PromiseWaker::wake() const {
  if (m_loop == xEventLoopCurrent()) {
    *m_done = true;
    if (m_fiber) {
      // Switch to the fiber on the spot — no event loop post needed.
      // wake() is always called on the main stack (fiber is suspended),
      // so swapcontext saves the current main-stack frame and jumps to
      // the fiber's saved context at park().  The main stack is frozen
      // in place; when the fiber later yields, it resumes here and the
      // call chain unwinds normally.
      xFiberSwitch(m_fiber);
    }
  } else {
    if (m_fiber) {
      xEventLoopPost(m_loop, &on_fiber_cross_thread_wake,
                     const_cast<void *>(static_cast<const void *>(this)));
    } else {
      xEventLoopPost(m_loop, &on_done,
                     const_cast<void *>(static_cast<const void *>(this)));
    }
  }
}

inline void PromiseWaker::park() const {
  while (!*m_done) {
    if (m_fiber) {
      xFiberYield();
    } else {
      xEventLoopRun(m_loop, X_RUN_ONCE);
    }
  }
  *m_done = false;
}

inline void PromiseWaker::on_fiber_cross_thread_wake(void *arg) {
  auto *w    = static_cast<PromiseWaker *>(arg);
  *w->m_done = true;
  xFiberSwitch(w->m_fiber);
}

inline void PromiseWaker::on_done(void *arg) {
  *static_cast<PromiseWaker *>(arg)->m_done = true;
}

#endif // XPP_FIBER

/* ═══════════════════════════════════════════════════════════════════════
 *  AtomicPromiseWaker
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Lock-free waker cell using a 2-bit state machine.
 *
 * Coordinates concurrent register_waker() (poll side) and wake()
 * (resolve side) without a mutex. Modeled after Tokio's AtomicWaker.
 *
 * The waker is stored in an Option — before the first register_waker()
 * call the cell is empty (None) and wake() is a no-op.
 *
 * State transitions:
 *   WAITING (00) ──CAS──→ REGISTERING (01)    register_waker acquires
 *   WAITING (00) ──fetch_or──→ WAKING (10)    wake acquires
 *   REGISTERING | WAKING (11)                 race: registerer self-wakes
 */
class AtomicPromiseWaker {
public:
  AtomicPromiseWaker() = default;

  AtomicPromiseWaker(const AtomicPromiseWaker &)            = delete;
  AtomicPromiseWaker &operator=(const AtomicPromiseWaker &) = delete;

  /**
   * @brief Store a waker for later notification.
   *
   * If a concurrent wake() is in flight, the waker is immediately
   * fired to prevent lost wakes.
   */
  void register_waker(PromiseWaker waker) {
    uint8_t expected = WAITING;
    if (m_state.compare_exchange_strong(expected, REGISTERING, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      m_waker      = std::move(waker);
      uint8_t prev = m_state.exchange(WAITING, std::memory_order_acq_rel);
      if (prev == (REGISTERING | WAKING)) {
        m_waker.unwrap().wake();
      }
    } else if (expected == WAKING) {
      waker.wake();
    }
  }

  /**
   * @brief Fire the stored waker if one is registered.
   *
   * If register_waker() is concurrently storing a waker, the
   * WAKING bit is set and register_waker() will self-wake on exit.
   */
  void wake() {
    uint8_t prev = m_state.fetch_or(WAKING, std::memory_order_acq_rel);
    if (prev == WAITING) {
      if (m_waker.is_some()) {
        m_waker.unwrap().wake();
      }
      m_state.store(WAITING, std::memory_order_release);
    }
  }

private:
  static constexpr uint8_t WAITING     = 0;
  static constexpr uint8_t REGISTERING = 0b01;
  static constexpr uint8_t WAKING      = 0b10;

  std::atomic<uint8_t>    m_state{WAITING};
  Option<PromiseWaker>    m_waker;
};

} // namespace xpp

#endif // XPP_PROMISE_WAKER_H
