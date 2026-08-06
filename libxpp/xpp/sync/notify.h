/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * notify.h - xpp::sync::Notify: reusable multi-waiter notification primitive.
 *
 * Use for waking coroutines that are waiting on some condition. Unlike
 * oneshot, Notify can be reused.
 *
 * When a notify arrives before a waiter is registered (as in the
 * cross-thread case where a worker notifies before the event-loop
 * coroutine calls notified()), the notification is accumulated as a
 * pending count. A subsequent call to notified() will consume it and
 * resolve immediately without registering a waiter.
 *
 * Mirrors tokio::sync::Notify: AtomicUsize(state|count) outside the
 * mutex with Mutex<WaitList> protecting only the waiter list.
 */

#ifndef XPP_SYNC_NOTIFY_H
#define XPP_SYNC_NOTIFY_H

#include <xpp/loom/mutex.h>
#include <xpp/promise.h>
#include <xpp/vec.h>
#include <xpp/loom/internal.h>

namespace xpp {
namespace sync {

/**
 * @brief Reusable, multi-waiter notification primitive.
 *
 * A low-level synchronization primitive used to wake one or more
 * coroutines waiting on a condition. Notify accumulates pending
 * notifications when no waiters are registered, so a subsequent
 * notified() call will resolve immediately.
 *
 * Mirrors the design of tokio::sync::Notify: an AtomicUsize for
 * the pending count and a Mutex-protected wait list.
 *
 * @see xpp::sync::broadcast::Receiver::recv(), xpp::sync::watch::Receiver::changed()
 */
class Notify {
public:
  Notify() = default;

  /**
   * @brief Wait for the next notification, or return immediately if
   *        a pending notification already exists.
   */
  xpp::Promise<void> notified() {
    // Fast path: consume a pending notification without taking the lock.
    if (m_pending.load(std::memory_order_acquire) > 0) {
      m_pending.fetch_sub(1, std::memory_order_release);
      auto [p, r] = xpp::async<void>();
      r.resolve();
      return std::move(p);
    }

    auto g = m_waiters.lock();
    // Re-check under lock. A concurrent notify may have arrived between
    // the fast-path check and acquiring the mutex.
    if (m_pending.load(std::memory_order_relaxed) > 0) {
      m_pending.fetch_sub(1, std::memory_order_relaxed);
      auto [p, r] = xpp::async<void>();
      r.resolve();
      return std::move(p);
    }
    auto [p, r] = xpp::async<void>();
    g->push(std::move(r));
    return std::move(p);
  }

  /**
   * @brief Wake exactly one waiting coroutine, or accumulate a pending
   *        notification if none are waiting.
   *
   * At most one waiter is released. If no waiters are registered, the
   * notification is stored as a pending count so the next notified()
   * caller resolves immediately.
   */
  void notify_one() {
    xpp::PromiseResolver<void> r;
    {
      auto g = m_waiters.lock();
      if (!g->empty()) {
        r = std::move(g->pop().unwrap());
      }
    } // lock released
    if (r.is_pending()) {
      r.resolve();
    } else {
      m_pending.fetch_add(1, std::memory_order_release);
    }
  }

  /**
   * @brief Wake all currently waiting coroutines.
   *
   * Every registered waiter is released. If no waiters are registered,
   * a pending notification is accumulated instead so the next
   * notified() call resolves immediately.
   */
  void notify_waiters() {
    Vec<xpp::PromiseResolver<void>> waiters;
    {
      auto g = m_waiters.lock();
      waiters = std::move(*g);
      g->clear();
    }
    if (!waiters.empty()) {
      for (auto &w : waiters) {
        w.resolve();
      }
    } else {
      m_pending.fetch_add(1, std::memory_order_release);
    }
  }

private:
  // TODO: replace with a lock-free linked list, and fold m_pending into
  // a single AtomicUsize with state bits, matching tokio's pattern.
  xpp::loom::Mutex<Vec<xpp::PromiseResolver<void>>> m_waiters;
  xpp::loom::_::Atomic<size_t>                              m_pending{0};
};

} // namespace sync
} // namespace xpp

#endif // XPP_SYNC_NOTIFY_H
