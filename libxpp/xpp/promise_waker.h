/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_waker.h - PromiseWaker + AtomicPromiseWaker.
 *
 * PromiseWaker captures an event loop handle + done flag. When fired,
 * it sets done = true — directly if same-thread, or via xEventLoopPost
 * if cross-thread.
 *
 * The done flag is owned inline (m_storage). Copies share the same flag
 * via a pointer — a waker held by a promise node and the original in
 * wait() both read/write the same bool.
 *
 * Fiber support (XPP_FIBER): when created inside a fiber, the waker
 * remembers the fiber handle. park() suspends the fiber instead of
 * blocking the event loop. wake() on the event loop boundary switches
 * back to the fiber after setting the done flag.
 *
 * The done flag is a pointer to m_storage rather than an inline bool.
 * This is necessary because PromiseWaker is copyable, and copies must
 * share the same flag — AtomicPromiseWaker stores a copy of the waker,
 * and its wake() must visibly set the flag that wait()'s park() checks.
 *
 * sizeof(PromiseWaker): 24B (32B with XPP_FIBER).
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_WAKER_H
#define XPP_PROMISE_WAKER_H

#include <atomic>

#include <xpp/event.h>

#if XPP_FIBER
#include <x/base/fiber.h>
#endif

namespace xpp {

/**
 * @brief Lightweight waker — captures event loop + done flag + optional fiber.
 *
 * When fired, sets the internal done flag. Same-thread wakes set it
 * directly. Cross-thread wakes post to the loop's done queue.
 *
 * park() blocks the caller until the flag is set, then resets it:
 *   - Non-fiber: runs xEventLoopRun(X_RUN_ONCE) in a poll loop.
 *   - Fiber:     calls xFiberSwitch(xFiberMain()) to suspend and
 *                yield the CPU back to the event loop.
 *
 * The constructor auto-detects the execution context via xFiberCurrent().
 * No factory method needed — just instantiate it.
 */
class PromiseWaker {
public:
  /**
   * @brief Construct a waker, auto-detecting the execution context.
   *
   * Captures the event loop handle from xEventLoopCurrent() if available
   * (i.e. inside a WaitScope).  If called outside a WaitScope — as is
   * the case for placeholder wakers inside AtomicPromiseWaker — the
   * waker is inert (m_loop == nullptr, wake() and park() are no-ops).
   *
   * When XPP_FIBER is enabled, also captures the current fiber handle
   * via xFiberCurrent() for cooperative parking.
   */
  PromiseWaker() : m_done(&m_storage) {
    m_loop = xEventLoopCurrent();
#if XPP_FIBER
    m_fiber = m_loop ? xFiberCurrent() : nullptr;
#endif
  }

  /**
   * @brief Notify the waiter that the promise may be ready.
   *
   * Sets the internal done flag. If the waker was created inside a
   * fiber, also posts a callback to switch back to the fiber on the
   * event loop boundary. Fiber switch is deferred via post rather than
   * inline because wake() may be called from deep inside poll/resolve.
   */
  void wake() const {
    if (!m_loop) return;
    if (m_loop == xEventLoopCurrent()) {
      // Same thread — set flag directly.
      *m_done = true;
#if XPP_FIBER
      if (m_fiber) {
        // Post the fiber switch to the event loop boundary.  Don't
        // switch inline — wake() may be called from deep inside
        // poll() / resolve(), where swapcontext is unsafe.
        xEventLoopPost(m_loop, &on_fiber_wake, const_cast<PromiseWaker *>(this));
      }
#endif
    } else {
      // Cross-thread — post to the loop's done queue.
#if XPP_FIBER
      if (m_fiber) {
        xEventLoopPost(m_loop, &on_fiber_cross_thread_wake,
                       const_cast<void *>(static_cast<const void *>(this)));
      } else
#endif
      {
        xEventLoopPost(m_loop, &set_done_cb, const_cast<void *>(static_cast<const void *>(this)));
      }
    }
  }

  /**
   * @brief Park the current context until wake() is called.
   *
   * Non-fiber path: runs xEventLoopRun(X_RUN_ONCE) in a busy-wait
   * loop until the done flag is set, then resets it.
   *
   * Fiber path: suspends the current fiber via xFiberSwitch(main),
   * yielding the CPU to the event loop. When wake() fires, the fiber
   * is switched back in and this call returns.
   *
   * After returning, the done flag is reset to false so the caller
   * can re-poll and potentially wait again.
   *
   * Must only be called when a valid event loop is captured
   * (i.e. inside a WaitScope). Calling on an inert waker (no loop)
   * is a no-op — the flag was presumably already set.
   */
  void park() const {
    while (!*m_done) {
#if XPP_FIBER
      if (m_fiber) {
        xFiberSwitch(xFiberMain());
        continue;
      }
#endif
      if (m_loop) {
        xEventLoopRun(m_loop, X_RUN_ONCE);
      } else {
        // If no loop and not a fiber, this is an inert waker — the
        // flag should already be set (or will be set externally).
        // Busy-waiting without an event loop would deadlock, so we
        // break out and let the caller handle the empty result.
        break;
      }
    }
    *m_done = false;
  }

private:
  xEventLoop m_loop;
  bool      *m_done;
  bool       m_storage = false;
#if XPP_FIBER
  xFiber m_fiber = nullptr;

  static void on_fiber_wake(void *arg) {
    auto *w = static_cast<PromiseWaker *>(arg);
    xFiberSwitch(w->m_fiber);
  }

  static void on_fiber_cross_thread_wake(void *arg) {
    auto *w    = static_cast<PromiseWaker *>(arg);
    *w->m_done = true;
    xFiberSwitch(w->m_fiber);
  }
#endif

  static void set_done_cb(void *arg) {
    *static_cast<PromiseWaker *>(arg)->m_done = true;
  }
};

/**
 * @brief Lock-free waker cell using a 2-bit state machine.
 *
 * Coordinates concurrent register_waker() (poll side) and wake()
 * (resolve side) without a mutex. Modeled after Tokio's AtomicWaker.
 *
 * State transitions:
 *   WAITING (00) ──CAS──→ REGISTERING (01)    register_waker acquires
 *   WAITING (00) ──fetch_or──→ WAKING (10)    wake acquires
 *   REGISTERING | WAKING (11)                 race: registerer self-wakes
 *
 * The waker cell is only accessed by whichever thread holds exclusive
 * access (REGISTERING or WAKING bit).
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
        m_waker.wake();
      }
    } else if (expected == WAKING) {
      waker.wake();
    }
  }

  /**
   * @brief Fire the stored waker.
   *
   * If register_waker() is concurrently storing a waker, the
   * WAKING bit is set and register_waker() will self-wake on exit.
   */
  void wake() {
    uint8_t prev = m_state.fetch_or(WAKING, std::memory_order_acq_rel);
    if (prev == WAITING) {
      m_waker.wake();
      m_state.store(WAITING, std::memory_order_release);
    }
  }

private:
  static constexpr uint8_t WAITING     = 0;
  static constexpr uint8_t REGISTERING = 0b01;
  static constexpr uint8_t WAKING      = 0b10;

  std::atomic<uint8_t> m_state{WAITING};
  PromiseWaker         m_waker;
};

} // namespace xpp

#endif // XPP_PROMISE_WAKER_H
