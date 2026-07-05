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
 */

#ifndef XPP_SYNC_NOTIFY_H
#define XPP_SYNC_NOTIFY_H

#include <vector>

#include <xpp/promise.h>
#include <xpp/sync/internal.h>

namespace xpp {
namespace sync {

class Notify {
public:
  Notify() = default;

  /**
   * @brief Wait for the next notification, or return immediately if
   *        a pending notification already exists.
   *
   * @par Optimization opportunity
   * The slow path (registering a waiter) currently takes a mutex to push
   * into a std::vector. A fully lock-free implementation would follow
   * tokio's pattern: pack EMPTY / WAITING / NOTIFIED state bits plus a
   * pending-count increment into a single atomic word, with lock-free
   * linked-list waiter nodes. This eliminates the mutex entirely on
   * both the notified() and notify() paths.
   */
  xpp::Promise<void> notified() {
    // Fast path: consume a pending notification without taking the lock.
    if (m_pending.load(std::memory_order_acquire) > 0) {
      m_pending.fetch_sub(1, std::memory_order_release);
      auto [p, r] = xpp::async<void>();
      r.resolve();
      return std::move(p);
    }

    xpp::sync::_::Lock lock(m_mutex);
    // Re-check under lock. A concurrent notify may have arrived between
    // the fast-path check and acquiring the mutex.
    if (m_pending.load(std::memory_order_relaxed) > 0) {
      m_pending.fetch_sub(1, std::memory_order_relaxed);
      auto [p, r] = xpp::async<void>();
      r.resolve();
      return std::move(p);
    }
    auto [p, r] = xpp::async<void>();
    m_waiters.emplace_back(std::move(r));
    return std::move(p);
  }

  void notify_one() {
    xpp::sync::_::Lock lock(m_mutex);
    if (!m_waiters.empty()) {
      auto r = std::move(m_waiters.back());
      m_waiters.pop_back();
      lock.unlock();
      r.resolve();
    } else {
      m_pending.fetch_add(1, std::memory_order_release);
    }
  }

  void notify_waiters() {
    xpp::sync::_::Lock lock(m_mutex);
    if (!m_waiters.empty()) {
      auto waiters = std::move(m_waiters);
      m_waiters.clear();
      lock.unlock();
      for (auto &w : waiters) {
        w.resolve();
      }
    } else {
      m_pending.fetch_add(1, std::memory_order_release);
    }
  }

private:
  // TODO: replace m_waiters with a lock-free linked list, and fold
  // m_pending into a single atomic word with state bits, matching
  // tokio's AtomicUsize(state | notified_count) pattern.
  xpp::sync::_::Mutex                     m_mutex;
  std::vector<xpp::PromiseResolver<void>> m_waiters;
  xpp::sync::_::Atomic<size_t>            m_pending{0};
};

} // namespace sync
} // namespace xpp

#endif // XPP_SYNC_NOTIFY_H
