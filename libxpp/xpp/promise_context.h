/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_context.h - PromiseContext (Rust's Context)
 *
 * PromiseContext is the non-cloneable handle passed to PromiseNode::poll().
 * It owns a PromiseWaker and exposes it via waker().  The copy constructor
 * is deleted — PromiseContext only lives on the await() stack frame; to
 * store a wake handle, extract the PromiseWaker via cx.waker() and clone
 * that.
 *
 * PromiseContext also provides park() — the await-side wait mechanism:
 *   - Fiber:     suspends via xFiberYield() (single yield, no loop —
 *                xFiberYield only returns when someone calls
 *                xFiberSwitch on this fiber, i.e. PromiseWaker::wake).
 *   - Non-fiber: runs xEventLoopRun(X_RUN_ONCE) in a poll loop until
 *                the waker's done flag is set.
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_CONTEXT_H
#define XPP_PROMISE_CONTEXT_H

#include <xpp/event.h>
#include <xpp/promise_waker.h>

#if XPP_FIBER
#include <x/base/fiber.h>
#endif

namespace xpp {

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseContext — declaration
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Non-cloneable poll context. Owns a PromiseWaker.
 *
 * Passed by const reference to PromiseNode::poll().  Callers who need
 * a storable wake handle must extract it explicitly:
 *
 *   void poll(const PromiseContext &cx) {
 *     s.atomic_waker.register_by_ref(cx.waker());  // borrow + store copy
 *   }
 *
 * The copy constructor is deleted so PromiseContext cannot be accidentally
 * stored — it only lives for the duration of a poll() call.
 */
class PromiseContext {
public:
  PromiseContext(const PromiseContext &)            = delete;
  PromiseContext &operator=(const PromiseContext &) = delete;

  /** @brief The wake handle for this poll. Clone to store. */
  const PromiseWaker &waker() const {
    return m_waker;
  }

  /** @brief Park until waker().wake() is called. */
  void park() const;

  /** @brief Construct a PromiseContext bound to the current event loop / fiber. */
  PromiseContext();

private:
  PromiseWaker m_waker;
};

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseContext — implementation
 * ═══════════════════════════════════════════════════════════════════════ */

inline PromiseContext::PromiseContext() : m_waker(PromiseWaker::make()) {}

inline void PromiseContext::park() const {
#if XPP_FIBER
  if (m_waker.m_core->fiber) {
    /* Fiber path: a single xFiberYield is sufficient — xFiberYield
     * only returns when PromiseWaker::wake() calls xFiberSwitch on
     * this fiber.  No done flag, no loop. */
    xFiberYield();
    return;
  }
#endif
  /* Non-fiber path: run the event loop until wake() sets done. */
  while (!m_waker.m_core->done) {
    xEventLoopRun(m_waker.m_core->loop, X_RUN_ONCE);
  }
  m_waker.m_core->done = false;
}

} // namespace xpp

#endif // XPP_PROMISE_CONTEXT_H
