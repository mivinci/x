/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_context.h - PromiseContext + AtomicPromiseWaker
 *
 * PromiseContext is the await-side handle that coordinates suspend/wake
 * between a wait-er and a resolver.  It holds an Arc<PromiseWaker> so
 * all copies (the local one on the await() stack and any copy stored
 * inside AtomicPromiseWaker) share the same state.
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
 *   _::PromiseWaker  — inner state {loop, done, fiber}  (Rust: Waker)
 *   PromiseContext   — public handle                     (Rust: Context)
 *
 * sizeof(PromiseContext) = sizeof(Arc<PromiseWaker>) = 8B.
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_CONTEXT_H
#define XPP_PROMISE_CONTEXT_H

#include <atomic>

#include <xpp/arc.h>
#include <xpp/event.h>
#include <xpp/option.h>
#include <xpp/promise_types.h>

#if XPP_FIBER
#include <x/base/fiber.h>
#endif

namespace xpp {

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseContext
 * ═══════════════════════════════════════════════════════════════════════ */

class PromiseContext {
public:
  PromiseContext();
  void wake() const;
  void park() const;

private:
  Arc<_::PromiseWaker> core;

  static Arc<_::PromiseWaker> make_core();
  static void on_wake(void *arg);
};

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseContext — implementation
 * ═══════════════════════════════════════════════════════════════════════ */

inline PromiseContext::PromiseContext() : core(make_core()) {}

inline Arc<_::PromiseWaker> PromiseContext::make_core() {
#if XPP_FIBER
  auto f = xFiberCurrent();
  if (f) {
    void *raw = xFiberProcArg(f);
    if (raw) {
      return static_cast<_::fiber::Context *>(raw)->waker;
    }
  }
#endif
  return Arc<_::PromiseWaker>::make(xEventLoopCurrent());
}

inline void PromiseContext::wake() const {
  if (core->loop == xEventLoopCurrent()) {
#if XPP_FIBER
    if (core->fiber) {
      xFiberSwitch(core->fiber);
      return;
    }
#endif
    core->done = true;
  } else {
    xEventLoopPost(core->loop, &on_wake,
                   const_cast<void *>(static_cast<const void *>(&(*core))));
  }
}

inline void PromiseContext::park() const {
  while (!core->done) {
#if XPP_FIBER
    if (core->fiber) {
      xFiberYield();
    } else
#endif
    {
      xEventLoopRun(core->loop, X_RUN_ONCE);
    }
  }
  core->done = false;
}

inline void PromiseContext::on_wake(void *arg) {
  auto *c = static_cast<_::PromiseWaker *>(arg);
  c->done = true;
#if XPP_FIBER
  if (c->fiber) {
    xFiberSwitch(c->fiber);
  }
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
 *  AtomicPromiseWaker
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Lock-free waker cell using a 2-bit state machine.
 *
 * Coordinates concurrent register() (poll side) and wake()
 * (resolve side) without a mutex. Modeled after Tokio's AtomicWaker.
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

  void register_(PromiseContext cx) {
    uint8_t expected = WAITING;
    if (m_state.compare_exchange_strong(expected, REGISTERING, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      m_context   = std::move(cx);
      uint8_t prev = m_state.exchange(WAITING, std::memory_order_acq_rel);
      if (prev == (REGISTERING | WAKING)) {
        m_context.unwrap().wake();
      }
    } else if (expected == WAKING) {
      cx.wake();
    }
  }

  void wake() {
    uint8_t prev = m_state.fetch_or(WAKING, std::memory_order_acq_rel);
    if (prev == WAITING) {
      if (m_context.is_some()) {
        m_context.unwrap().wake();
      }
      m_state.store(WAITING, std::memory_order_release);
    }
  }

private:
  static constexpr uint8_t WAITING     = 0;
  static constexpr uint8_t REGISTERING = 0b01;
  static constexpr uint8_t WAKING      = 0b10;

  std::atomic<uint8_t>    m_state{WAITING};
  Option<PromiseContext>  m_context;
};

} // namespace xpp

#endif // XPP_PROMISE_CONTEXT_H
