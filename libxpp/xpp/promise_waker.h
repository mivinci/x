/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_waker.h - WakeState + PromiseWaker + AtomicPromiseWaker
 *
 * _::WakeState is the inner shared state {loop, woken, fiber} allocated
 * on the heap and shared via Arc.  PromiseWaker wraps the Arc and is
 * the cloneable, storable wake handle (Rust's Waker).
 *
 * In fiber mode (XPP_FIBER), the Arc is allocated *once* per fiber
 * (by xpp::fiber()) and stored in _::fiber::Context.  Every await()
 * clones it — zero heap allocation, just an atomic fetch_add.
 * xFiberProcArg() + _::fiber::Context::waker provide direct access
 * (no offset tricks, no TLS registry).
 *
 * Non-fiber mode allocates a new Arc per await() (the pre-existing
 * behaviour — this is rarely used and acceptable).
 *
 * Naming (aligned with Rust):
 *   _::WakeState       — inner state {loop, woken, fiber}
 *   PromiseWaker       — public cloneable handle           (Rust: Waker)
 *   PromiseContext     — non-cloneable poll handle          (Rust: Context)
 *                        (see <xpp/promise_context.h>)
 *
 * sizeof(PromiseWaker) = sizeof(Arc<WakeState>) = 8B.
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

class PromiseContext; // forward — friend declaration

/* ═══════════════════════════════════════════════════════════════════════
 *  _::WakeState — inner waker state (shared via Arc)
 * ═══════════════════════════════════════════════════════════════════════ */

namespace _ {

struct WakeState {
  xEventLoop loop;
  bool       woken = false;
  /* Optional wake callback (used by xpp::spawn): invoked (via
   * xEventLoopPost) whenever wake() fires, so a poll-driven promise
   * chain can be re-polled without a blocking await loop. NULL for
   * regular await() users — behaviour unchanged.
   *
   * Ownership protocol (makes late/duplicate wakes safe):
   * - wake_retain/wake_release, when set, manage the lifetime of
   *   wake_arg: do_wake() retains before posting, the posted callback
   *   releases when done, and ~WakeState releases the reference the
   *   waker itself holds. A wake aimed at an already-completed chain
   *   is then a harmless no-op instead of a use-after-free. */
  void (*wake_cb)(void *)      = nullptr;
  void (*wake_retain)(void *)  = nullptr;
  void (*wake_release)(void *) = nullptr;
  std::atomic<void *> wake_arg{nullptr};

  WakeState() = default;
  explicit WakeState(xEventLoop l) : loop(l) {}
  WakeState(xEventLoop l, void (*cb)(void *), void *arg) : loop(l), wake_cb(cb), wake_arg(arg) {}
  ~WakeState() {
    if (wake_release) wake_release(wake_arg.load(std::memory_order_acquire));
  }

#if XPP_FIBER
  xFiber fiber = nullptr;
  WakeState(xEventLoop l, xFiber f) : loop(l), fiber(f) {}
#endif
};

} // namespace _

/* ═══════════════════════════════════════════════════════════════════════
 *  _::fiber::Context — per-fiber execution context header
 * ═══════════════════════════════════════════════════════════════════════ */

#if XPP_FIBER
namespace _ {
namespace fiber {

/**
 * @brief Per-fiber execution context.  Lives on the heap, allocated once
 *        by xpp::fiber().  The waker field carries the per-fiber
 *        PromiseWaker — every await() inside the fiber clones its Arc
 *        (1 fetch_add, 0 heap alloc).
 */
struct Context {
  void (*run)(void *state);
  void (*destroy)(void *state);
  xFiber            handle;
  Arc<_::WakeState> waker;
};

} // namespace fiber
} // namespace _
#endif

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — declaration
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Cloneable wake handle. Wraps Arc<_::WakeState>.
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
  static PromiseWaker create();

  /**
   * @brief Create a waker that re-posts @p cb to the event loop on every
   *        wake (in addition to setting the woken flag). Used by xpp::spawn
   *        to drive a promise chain without a blocking await loop.
   */
  static PromiseWaker create_with_wake_cb(void (*cb)(void *), void *arg);

  /**
   * @brief Attach an owned wake-callback argument (used by spawn to point
   *        the callback at its driver state).
   *
   * Takes over @p arg: this waker holds one reference (released when the
   * last copy of the shared WakeState dies, via @p release), and every
   * posted callback invocation holds one (@p retain is taken before each
   * post; the callback releases when done). Makes late/duplicate wakes
   * safe: a callback posted for an already-completed chain is a no-op.
   */
  void attach_wake_arg(void *arg, void (*retain)(void *), void (*release)(void *)) {
    m_state->wake_retain  = retain;
    m_state->wake_release = release;
    m_state->wake_arg.store(arg, std::memory_order_release);
    retain(arg); // the waker's own ownership reference
  }

  /** @brief Drop this handle's share of the shared state (keeps copies). */
  void reset() {
    Arc<_::WakeState> released(
      std::move(m_state)); // m_state is now null; `released` frees on scope exit
  }

private:
  explicit PromiseWaker(Arc<_::WakeState> state) : m_state(std::move(state)) {}
  Arc<_::WakeState> m_state;

  static void on_wake(void *arg);
  static void on_wake_owned(void *arg);

  friend class PromiseContext;
  friend class AtomicPromiseWaker;
};

/* ═══════════════════════════════════════════════════════════════════════
 *  PromiseWaker — implementation
 * ═══════════════════════════════════════════════════════════════════════ */

namespace _ {
/// Heap token for a cross-thread wake post: owns one PromiseWaker (an
/// Arc copy) so the shared WakeState outlives the queued callback even
/// if every other copy is dropped in the meantime.
struct WakeToken {
  PromiseWaker waker;
};
} // namespace _

inline PromiseWaker PromiseWaker::create_with_wake_cb(void (*cb)(void *), void *arg) {
  return PromiseWaker(Arc<_::WakeState>::make(xEventLoopCurrent(), cb, arg));
}

inline PromiseWaker PromiseWaker::create() {
#if XPP_FIBER
  auto f = xFiberCurrent();
  if (f) {
    void *raw = xFiberProcArg(f);
    if (raw) {
      return PromiseWaker(static_cast<_::fiber::Context *>(raw)->waker);
    }
  }
#endif
  return PromiseWaker(Arc<_::WakeState>::make(xEventLoopCurrent()));
}

/// Unified wake action: set the woken flag (for await's park loop) and,
/// if a wake callback is registered (for spawn), re-post it to the loop.
/// The post owns a reference to the callback argument (retain/release
/// protocol), so a callback arriving after its chain completed is safe.
static inline void do_wake(_::WakeState *s) {
  s->woken = true;
  if (s->wake_cb) {
    void *arg = s->wake_arg.load(std::memory_order_acquire);
    if (s->wake_retain) s->wake_retain(arg);
    if (xEventLoopPost(s->loop, s->wake_cb, arg) != xErrno_Ok) {
      if (s->wake_release) s->wake_release(arg);
    }
  }
}

inline void PromiseWaker::wake() const {
  if (m_state->loop == xEventLoopCurrent()) {
#if XPP_FIBER
    if (m_state->fiber) {
      xFiberSwitch(m_state->fiber);
      return;
    }
#endif
    do_wake(m_state.get());
  } else {
    // Cross-thread wake: the post must own a reference to the shared
    // state — the awaiting side may drop all its copies before the
    // target loop runs the callback. Pack this handle (an Arc copy)
    // into a small token that the callback deletes after firing.
    auto *tok = new _::WakeToken{*this};
    if (xEventLoopPost(m_state->loop, &PromiseWaker::on_wake_owned, tok) != xErrno_Ok) delete tok;
  }
}

inline void PromiseWaker::on_wake(void *arg) {
  auto *c = static_cast<_::WakeState *>(arg);
#if XPP_FIBER
  if (c->fiber) {
    xFiberSwitch(c->fiber);
    return;
  }
#endif
  do_wake(c);
}

inline void PromiseWaker::on_wake_owned(void *arg) {
  auto *tok = static_cast<_::WakeToken *>(arg);
  on_wake(static_cast<void *>(&*tok->waker.m_state));
  delete tok;
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

  /**
   * @brief Remove and return the registered waker (tokio's take_waker).
   *
   * Used by poll nodes whose post-registration re-check succeeded: they
   * no longer depend on wake delivery, so the stale registration is
   * cleared instead of being fired later — a late fire would target a
   * possibly-completed chain (harmless no-op) and, worse, may post to
   * an event loop that no longer exists. If a wake() is concurrently
   * in flight, the waker is left in place (the wake is then delivered
   * as usual — a spurious re-poll of a node that already succeeded).
   */
  Option<PromiseWaker> take_waker() {
    uint8_t expected = WAITING;
    if (!m_state.compare_exchange_strong(expected, REGISTERING, std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
      return none; // WAKING in flight (REGISTERING impossible: single poller)
    }
    Option<PromiseWaker> w = m_waker;
    m_waker                = none;
    m_state.store(WAITING, std::memory_order_release);
    return w;
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
