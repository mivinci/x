/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_waker.h - PromiseWaker + PromiseAtomicWaker.
 *
 * PromiseWaker captures an event loop handle + flag pointer. When
 * fired, it sets *done = true — directly if same-thread, or via
 * xEventLoopPost if cross-thread.
 *
 * PromiseAtomicWaker is a lock-free waker cell that coordinates
 * concurrent register/wake without a mutex. Modeled after Tokio's
 * AtomicWaker.
 *
 * sizeof(PromiseWaker) == 2 * sizeof(void*) == 16 bytes.
 * Trivially copyable.
 *
 * C++17-compatible.
 */

#ifndef XPP_PROMISE_WAKER_H
#define XPP_PROMISE_WAKER_H

#include <atomic>

#include <xpp/event.h>

namespace xpp {

/**
 * @brief Lightweight waker — captures an event loop + flag pointer.
 *
 * When fired, sets *done = true. Same-thread wakes set the flag
 * directly (zero overhead). Cross-thread wakes post to the loop's
 * done queue via xEventLoopPost.
 *
 * The event loop handle is captured at creation time, so the waker
 * can be fired from any thread — including one that has not entered
 * a WaitScope.
 */
class PromiseWaker {
public:
  PromiseWaker() : m_loop(nullptr), m_done(nullptr) {}
  PromiseWaker(xEventLoop loop, bool *done) : m_loop(loop), m_done(done) {}

  void wake() const {
    if (!m_loop) return;
    if (m_loop == xEventLoopCurrent()) {
      // Same thread — set flag directly, skip the post overhead.
      *m_done = true;
    } else {
      // Cross-thread — post to the loop's done queue.
      xEventLoopPost(m_loop, [](void *a) { *static_cast<bool *>(a) = true; }, m_done);
    }
  }

  /**
   * @brief Create a waker that sets *done = true.
   *
   * Captures the event loop handle from EventLoop::current() at
   * creation time. The resulting waker is safe to fire from any
   * thread.
   *
   * Must be called within a WaitScope (so current() is valid).
   */
  static PromiseWaker sync_wait(bool *done) {
    return PromiseWaker(EventLoop::current(), done);
  }

private:
  xEventLoop m_loop;
  bool      *m_done;
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
class PromiseAtomicWaker {
public:
  PromiseAtomicWaker() = default;

  PromiseAtomicWaker(const PromiseAtomicWaker &)            = delete;
  PromiseAtomicWaker &operator=(const PromiseAtomicWaker &) = delete;

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
