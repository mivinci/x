/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_waker.h - PromiseWaker + AtomicPromiseWaker.
 *
 * PromiseWaker coordinates suspend/wake between a wait-er (await()) and a
 * resolver (PromiseResolver, another thread).  The state (loop, done flag,
 * optional fiber) lives in an Arc<WakerCore> so copies share the same data.
 *
 *   - Same-thread wake: sets done=true + optional xFiberSwitch
 *   - Cross-thread wake: xEventLoopPost → callback sets done=true +
 *     optional xFiberSwitch
 *   - park(): spins on done flag with xFiberYield / xEventLoopRun
 *
 * The done flag is reliably shared because PromiseWaker copies share the
 * identical Arc<WakerCore> — analogous to how Rust's Waker is an
 * Arc<dyn Wake> and clone shares the Arc.
 *
 * sizeof(PromiseWaker) = sizeof(Arc<WakerCore>) = sizeof(WakerCore*) = 8B.
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_WAKER_H
#define XPP_PROMISE_WAKER_H

#include <atomic>

#include <xpp/arc.h>
#include <xpp/event.h>
#include <xpp/option.h>

#if XPP_FIBER
#include <x/base/fiber.h>
#endif

namespace xpp {

/* ── WakerCore ────────────────────────────────────────────────────── */

namespace _ {

struct WakerCore {
  xEventLoop loop;
  bool       done = false;

  explicit WakerCore(xEventLoop l) : loop(l) {}

#if XPP_FIBER
  xFiber fiber = nullptr;
#endif
};

} // namespace _

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Waker — suspend/wake bridge between awaiter and resolver.
 *
 * Owns an Arc<WakerCore> so that copies (e.g. the one stored inside
 * AtomicPromiseWaker and the local one on the await() stack) share a
 * single done flag, loop handle, and fiber handle.  Cross-thread
 * callbacks posted via xEventLoopPost receive a raw pointer to the
 * WakerCore — safe as long as any Arc copy is alive.
 */
class PromiseWaker {
public:
  PromiseWaker();
  void wake() const;
  void park() const;

private:
  Arc<_::WakerCore> core;

  static void on_wake(void *arg);
};

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — implementation
 * ═══════════════════════════════════════════════════════════════════════ */

inline PromiseWaker::PromiseWaker()
    : core(Arc<_::WakerCore>::make(xEventLoopCurrent())) {
#if XPP_FIBER
  core->fiber = xFiberCurrent();
#endif
}

inline void PromiseWaker::wake() const {
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

inline void PromiseWaker::park() const {
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

inline void PromiseWaker::on_wake(void *arg) {
  auto *c = static_cast<_::WakerCore *>(arg);
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
