/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_waker.h - PromiseWaker + AtomicPromiseWaker
 *
 * PromiseWaker is the cloneable, storable wake handle (Rust's Waker).
 * It wraps an Arc<_::WakerCore> so all copies share the same heap state
 * via atomic reference counting.
 *
 * In fiber mode (XPP_FIBER), the Arc is allocated *once* per fiber
 * (by xpp::fiber()) and stored in _::fiber::Context.  Every await()
 * clones it — zero heap allocation, just an atomic fetch_add.
 * xFiberProcArg() + _::fiber::Context::waker provide direct access
 * (no offset tricks, no TLS registry — the struct is shared via
 * <xpp/promise_types.h>).
 *
 * Non-fiber mode allocates a new Arc per await() (the pre-existing
 * behaviour — this is rarely used and acceptable).
 *
 * Naming (aligned with Rust):
 *   _::WakerCore      — inner state {loop, done, fiber}
 *   PromiseWaker      — public cloneable handle           (Rust: Waker)
 *   PromiseContext    — non-cloneable poll handle          (Rust: Context)
 *                       (see <xpp/promise_context.h>)
 *
 * sizeof(PromiseWaker) = sizeof(Arc<WakerCore>) = 8B.
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_WAKER_H
#define XPP_PROMISE_WAKER_H

#include <atomic>

#include <xpp/arc.h>
#include <xpp/event.h>
#include <xpp/option.h>
#include <xpp/promise_types.h>

#if XPP_FIBER
#include <x/base/fiber.h>
#endif

namespace xpp {

class PromiseContext; // forward — friend declaration

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — declaration
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Cloneable wake handle. Wraps Arc<_::WakerCore>.
 *
 * Copies share the same inner state via atomic refcount (one fetch_add
 * per clone).  PromiseWaker is what resolvers store and fire;
 * PromiseContext is what awaiters construct and park on (PromiseContext
 * owns a PromiseWaker, exposes it via waker()).
 *
 * In fiber mode, PromiseWaker::make() reuses the per-fiber Arc allocated
 * by xpp::fiber() — 0 heap alloc per await(), just 1 fetch_add clone.
 */
class PromiseWaker {
public:
  PromiseWaker(const PromiseWaker &) noexcept            = default;
  PromiseWaker &operator=(const PromiseWaker &) noexcept = default;
  PromiseWaker(PromiseWaker &&) noexcept                 = default;
  PromiseWaker &operator=(PromiseWaker &&) noexcept      = default;

  /** @brief Notify the waiter that the promise may be ready. */
  void wake() const;

  /**
   * @brief Construct a PromiseWaker bound to the current event loop
   *        (and, in fiber mode, the current fiber).
   *
   * In fiber mode, reuses the per-fiber Arc if called from inside a
   * fiber created by xpp::fiber().  Otherwise allocates a fresh Arc.
   */
  static PromiseWaker make();

private:
  explicit PromiseWaker(Arc<_::WakerCore> c) : m_core(std::move(c)) {}
  Arc<_::WakerCore> m_core;

  static void on_wake(void *arg);

  friend class PromiseContext;
  friend class AtomicPromiseWaker;
};

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — implementation
 * ═══════════════════════════════════════════════════════════════════════ */

inline PromiseWaker PromiseWaker::make() {
#if XPP_FIBER
  auto f = xFiberCurrent();
  if (f) {
    void *raw = xFiberProcArg(f);
    if (raw) {
      return PromiseWaker(static_cast<_::fiber::Context *>(raw)->waker);
    }
  }
#endif
  return PromiseWaker(Arc<_::WakerCore>::make(xEventLoopCurrent()));
}

inline void PromiseWaker::wake() const {
  if (m_core->loop == xEventLoopCurrent()) {
#if XPP_FIBER
    if (m_core->fiber) {
      xFiberSwitch(m_core->fiber);
      return;
    }
#endif
    m_core->done = true;
  } else {
    xEventLoopPost(m_core->loop, &on_wake,
                   const_cast<void *>(static_cast<const void *>(&(*m_core))));
  }
}

inline void PromiseWaker::on_wake(void *arg) {
  auto *c = static_cast<_::WakerCore *>(arg);
#if XPP_FIBER
  if (c->fiber) {
    xFiberSwitch(c->fiber);
    return;
  }
#endif
  c->done = true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  AtomicPromiseWaker
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Lock-free waker cell using a 2-bit state machine.
 *
 * Coordinates concurrent register (poll side) and wake (resolve side)
 * without a mutex. Modeled after Tokio's AtomicWaker.
 *
 * State transitions:
 *   WAITING (00) ──CAS──→ REGISTERING (01)    register acquires
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
   * Takes a const reference — only borrows the caller's PromiseWaker;
   * copies it into storage only if the CAS succeeds.  If a concurrent
   * wake() is in flight, the waker is immediately fired to prevent
   * lost wakes.
   */
  void register_by_ref(const PromiseWaker &w) {
    uint8_t expected = WAITING;
    if (m_state.compare_exchange_strong(expected, REGISTERING, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      m_waker      = w;
      uint8_t prev = m_state.exchange(WAITING, std::memory_order_acq_rel);
      if (prev == (REGISTERING | WAKING)) {
        m_waker.unwrap().wake();
      }
    } else if (expected == WAKING) {
      w.wake();
    }
  }

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

  std::atomic<uint8_t> m_state{WAITING};
  Option<PromiseWaker> m_waker;
};

} // namespace xpp

#endif // XPP_PROMISE_WAKER_H
