/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * watch.h - xpp::sync::watch: single-value, version-tracked broadcast channel.
 *
 * Only the latest value is retained. Receivers independently track which
 * version they have "seen" via changed() or borrow_and_update().
 *
 * Uses xpp::loom::Mutex<T> — the Rust-style Mutex that owns its data.
 *
 * Aligned with tokio::sync::watch.
 */

#ifndef XPP_SYNC_WATCH_H
#define XPP_SYNC_WATCH_H

#include <utility>

#include <xpp/loom/mutex.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/shared.h>
#include <xpp/sync/notify.h>

namespace xpp {
namespace sync {
namespace watch {

namespace _ {

// ── Shared state ─────────────────────────────────────────────────────

template <class T> struct Value {
  T                              m_value;
  xpp::loom::_::Atomic<uint64_t> m_version{0};
  xpp::loom::_::Atomic<uint64_t> m_receiver_count{1};
  xpp::loom::_::Atomic<bool>     m_closed{false};

  template <typename... Args>
  explicit Value(Args &&...args) : m_value(std::forward<Args>(args)...) {}
};

} // namespace _

// ── Error types ──────────────────────────────────────────────────────

/**
 * @brief Error returned by Sender::send().
 *
 * Indicates that the send failed — typically because all receivers
 * had already been dropped when send() was called.
 *
 * @tparam T The value type of the channel.
 */
template <typename T> struct SendError {
  /** The error kind. */
  enum Kind {
    NoReceiver ///< All receivers have been dropped; no one will see the value.
  };
  Kind kind;  ///< The kind of error that occurred.
  T    value; ///< The value that was being sent, returned to the caller.
};

/** @brief Empty error type for Receiver::changed(). Indicates the channel is closed. */
struct RecvError {};

/**
 * @brief Read guard returned by borrow() / borrow_and_update().
 *
 * Holds an exclusive lock on the shared value, preventing concurrent
 * writes. In single-threaded builds the lock is a no-op.
 */
template <class T> class Ref {
public:
  Ref(Ref &&) noexcept            = default;
  Ref &operator=(Ref &&) noexcept = default;
  Ref(const Ref &)                = delete;
  Ref &operator=(const Ref &)     = delete;

  /** @brief Dereference the read guard to access the borrowed value. */
  const T &operator*() const noexcept {
    return (*m_guard).m_value;
  }
  /** @brief Arrow operator for direct member access on the borrowed value. */
  const T *operator->() const noexcept {
    return &(*m_guard).m_value;
  }

  explicit Ref(xpp::loom::MutexGuard<_::Value<T>> g) : m_guard(std::move(g)) {}

private:
  xpp::loom::MutexGuard<_::Value<T>> m_guard;
};

template <class T> class Receiver;

/**
 * @brief Sending half of a watch channel.
 *
 * Cloneable. Each send() replaces the stored value and wakes all
 * receivers. RAII close when the last Sender drops.
 */
template <class T> class Sender {
public:
  Sender(Sender &&) noexcept = default;
  Sender &operator=(Sender &&o) noexcept {
    if (this != &o) {
      drop();
      m_state = std::move(o.m_state);
    }
    return *this;
  }
  Sender(const Sender &o) : m_state(o.m_state) {
    if (m_state) m_state->m_sender_count.fetch_add(1, std::memory_order_relaxed);
  }
  Sender &operator=(const Sender &o) {
    if (this != &o) {
      drop();
      m_state = o.m_state;
      if (m_state) m_state->m_sender_count.fetch_add(1, std::memory_order_relaxed);
    }
    return *this;
  }
  ~Sender() {
    drop();
  }

  /**
   * @brief Send a new value, replacing the old one.
   * @return Ok(old) on success, Err(SendError) if no receivers.
   */
  xpp::Result<T, SendError<T>> send(T value);

  /// Lock and peek the current value.
  Ref<T> borrow() {
    return Ref<T>(m_state->m_value.lock());
  }

  /**
   * @brief Create a new Receiver subscribed to this channel.
   *
   * The new receiver sees the current value immediately and starts
   * tracking from the current version.
   *
   * @return A new Receiver<T> bound to the same shared state.
   */
  Receiver<T> subscribe();

  /**
   * @brief Number of active receivers.
   *
   * @return The current receiver count.
   */
  size_t receiver_count() const {
    return m_state ? m_state->m_value.lock()->m_receiver_count.load(std::memory_order_relaxed) : 0;
  }

  /**
   * @brief Check whether the channel has been closed.
   *
   * @return true if the last Sender has been dropped.
   */
  bool is_closed() const {
    return m_state && m_state->m_value.lock()->m_closed.load(std::memory_order_acquire);
  }

  /**
   * @brief Await until all receivers are dropped.
   *
   * Resolves when the receiver count drops to zero or the channel
   * is closed. Useful for graceful shutdown.
   */
  xpp::Promise<void> closed();

private:
  template <class U> friend class Receiver;
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(const U &);
  struct State {
    xpp::loom::Mutex<_::Value<T>>  m_value;
    xpp::loom::_::Atomic<uint64_t> m_sender_count{1};
    Notify                         m_notify;
    explicit State(const T &init) : m_value(init) {}
  };
  Shared<State> m_state;

  explicit Sender(Shared<State> s) : m_state(std::move(s)) {}

  void drop();
};

/**
 * @brief Receiving half of a watch channel.
 *
 * Each receiver independently tracks which version it has "seen".
 */
template <class T> class Receiver {
public:
  Receiver(Receiver &&) noexcept            = default;
  Receiver &operator=(Receiver &&) noexcept = default;
  Receiver(const Receiver &)                = delete;
  Receiver &operator=(const Receiver &)     = delete;

  ~Receiver() {
    if (!m_state) return;
    m_state->m_value.lock()->m_receiver_count.fetch_sub(1, std::memory_order_relaxed);
    m_state->m_notify.notify_waiters();
  }

  /**
   * @brief Wait for an unseen value.
   *
   * @return Ok if a new value arrived, Err(RecvError) if closed AND seen.
   */
  xpp::Promise<xpp::Result<void, RecvError>> changed();

  /// Synchronous check: is there an unseen value?
  xpp::Result<bool, RecvError> has_changed();

  /// Peek WITHOUT marking seen.
  Ref<T> borrow() {
    return Ref<T>(m_state->m_value.lock());
  }

  /// Peek AND mark seen.
  Ref<T> borrow_and_update();

private:
  template <class U> friend class Sender;
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(const U &);
  using State = typename Sender<T>::State;
  Shared<State> m_state;
  uint64_t      m_seen_version = 0;
  explicit Receiver(Shared<State> s) : m_state(std::move(s)) {}
};

// ── Deferred ─────────────────────────────────────────────────────────

template <class T> Receiver<T> Sender<T>::subscribe() {
  auto g = m_state->m_value.lock();
  g->m_receiver_count.fetch_add(1, std::memory_order_relaxed);
  auto rx           = Receiver<T>(m_state);
  rx.m_seen_version = g->m_version.load(std::memory_order_acquire);
  return rx;
}

/* ── closed() / changed() method bodies ────────────────────────────── */

#if XPP_HAS_COROUTINES

template <class T> xpp::Promise<void> Sender<T>::closed() {
  if (!m_state) co_return;

  // Check once under a single lock acquisition.
  {
    auto g = m_state->m_value.lock();
    if (g->m_receiver_count.load(std::memory_order_acquire) == 0) co_return;
    if (g->m_closed.load(std::memory_order_acquire)) co_return;
  }

  while (true) {
    {
      auto g = m_state->m_value.lock();
      if (g->m_receiver_count.load(std::memory_order_acquire) == 0) co_return;
      if (g->m_closed.load(std::memory_order_acquire)) co_return;
    }
    co_await m_state->m_notify.notified();
  }
}

template <class T> xpp::Promise<xpp::Result<void, RecvError>> Receiver<T>::changed() {
  if (!m_state) co_return xpp::err(RecvError{});

  // Fast path: already have an unseen value
  {
    auto g   = m_state->m_value.lock();
    uint64_t current = g->m_version.load(std::memory_order_acquire);
    if (current != m_seen_version) {
      m_seen_version = current;
      co_return xpp::ok;
    }
  }

  while (true) {
    uint64_t current;
    {
      auto g = m_state->m_value.lock();
      if (g->m_closed.load(std::memory_order_acquire))
        co_return xpp::err(RecvError{});
      current = g->m_version.load(std::memory_order_acquire);
    }
    if (current != m_seen_version) {
      m_seen_version = current;
      co_return xpp::ok;
    }
    co_await m_state->m_notify.notified();
  }
}

#else // !XPP_HAS_COROUTINES

#if XPP_FIBER

/* ═══ C++11 + fiber: linear while + .await() ═════════════════════════ */

template <class T> xpp::Promise<void> Sender<T>::closed() {
  if (!m_state) return xpp::resolve();

  {
    auto g = m_state->m_value.lock();
    if (g->m_receiver_count.load(std::memory_order_acquire) == 0) return xpp::resolve();
    if (g->m_closed.load(std::memory_order_acquire)) return xpp::resolve();
  }

  while (true) {
    {
      auto g = m_state->m_value.lock();
      if (g->m_receiver_count.load(std::memory_order_acquire) == 0) return xpp::resolve();
      if (g->m_closed.load(std::memory_order_acquire)) return xpp::resolve();
    }
    m_state->m_notify.notified().await();
  }
}

template <class T> xpp::Promise<xpp::Result<void, RecvError>> Receiver<T>::changed() {
  if (!m_state) return xpp::resolve(xpp::err(RecvError{}));

  uint64_t current = m_state->m_value.lock()->m_version.load(std::memory_order_acquire);
  if (current != m_seen_version) {
    m_seen_version = current;
    return xpp::resolve(xpp::ok);
  }

  while (true) {
    if (m_state->m_value.lock()->m_closed.load(std::memory_order_acquire))
      return xpp::resolve(xpp::err(RecvError{}));
    m_state->m_notify.notified().await();
    current = m_state->m_value.lock()->m_version.load(std::memory_order_acquire);
    if (current != m_seen_version) {
      m_seen_version = current;
      return xpp::resolve(xpp::ok);
    }
  }
}

#else // !XPP_FIBER

/**
 * C++11 closed(): lock-check-wait loop via recursive .then().
 *
 * Mutex is NOT held during notify waits — each iteration locks,
 * checks the condition, unlocks, then chains onto notified().
 * The guard is a stack variable, naturally dropped at scope exit.
 *
 * No extra heap allocation. Promise chain nodes use arena bump alloc.
 */
template <class T> xpp::Promise<void> Sender<T>::closed() {
  if (!m_state) return xpp::resolve();

  {
    auto g = m_state->m_value.lock();
    if (g->m_receiver_count.load(std::memory_order_acquire) == 0) return xpp::resolve();
    if (g->m_closed.load(std::memory_order_acquire)) return xpp::resolve();
  } // mutex released

  struct ClosedLoop {
    Shared<State> state;

    Promise<void> operator()() {
      {
        auto g = state->m_value.lock();
        if (g->m_receiver_count.load(std::memory_order_acquire) == 0) return xpp::resolve();
        if (g->m_closed.load(std::memory_order_acquire)) return xpp::resolve();
      }
      return state->m_notify.notified().then(
        [self = std::move(*this)](Void) mutable { return self(); });
    }
  };

  return ClosedLoop{m_state}();
}

/**
 * C++11 changed(): wait for a version bump via recursive .then().
 *
 * The seen version is stored in Shared<uint64_t> so it can survive
 * across .then() callback boundaries. After the chain resolves,
 * the final .then() writes the version back to m_seen_version.
 *
 * Extra allocation: 1 Shared<uint64_t> (8 bytes heap refcount).
 */
template <class T> xpp::Promise<xpp::Result<void, RecvError>> Receiver<T>::changed() {
  if (!m_state) return xpp::resolve(xpp::err(RecvError{}));

  // Fast path: already have an unseen value
  uint64_t current = m_state->m_value.lock()->m_version.load(std::memory_order_acquire);
  if (current != m_seen_version) {
    m_seen_version = current;
    return xpp::resolve(xpp::ok);
  }

  // Slow path: wait loop. Version is shared so the struct can update it.
  auto pver = Shared<uint64_t>::make(m_seen_version);

  struct ChangedLoop {
    Shared<State>    state;
    Shared<uint64_t> pver;

    xpp::Promise<xpp::Result<void, RecvError>> operator()() {
      uint64_t current;
      {
        auto g  = state->m_value.lock();
        current = g->m_version.load(std::memory_order_acquire);
        if (current != *pver) {
          *pver = current;
          return xpp::resolve(xpp::ok);
        }
        if (g->m_closed.load(std::memory_order_acquire)) return xpp::resolve(xpp::err(RecvError{}));
      }
      return state->m_notify.notified().then(
        [self = std::move(*this)](Void) mutable { return self(); });
    }
  };

  return ChangedLoop{m_state, pver}().then([this, pver](xpp::Result<void, RecvError> r) {
    m_seen_version = *pver; // sync back the may-have-changed version
    return xpp::resolve(std::move(r));
  });
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

/* ── Synchronous methods (C++11, no coroutine dependency) ──────────── */

template <class T> xpp::Result<T, SendError<T>> Sender<T>::send(T value) {
  if (!m_state) return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

  auto  g = m_state->m_value.lock();
  auto &v = *g;
  if (v.m_closed.load(std::memory_order_acquire))
    return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});
  if (v.m_receiver_count.load(std::memory_order_acquire) == 0)
    return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

  T old     = std::move(v.m_value);
  v.m_value = std::move(value);
  v.m_version.fetch_add(1, std::memory_order_release);
  // g drops here → mutex unlocked

  m_state->m_notify.notify_waiters();
  return xpp::ok(std::move(old));
}

template <class T> void Sender<T>::drop() {
  if (!m_state) return;
  if (m_state->m_sender_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    {
      auto g = m_state->m_value.lock();
      g->m_closed.store(true, std::memory_order_release);
    }
    m_state->m_notify.notify_waiters();
  }
}

template <class T> xpp::Result<bool, RecvError> Receiver<T>::has_changed() {
  if (!m_state) return xpp::err(RecvError{});
  auto g = m_state->m_value.lock();
  if (g->m_closed.load(std::memory_order_acquire)) return xpp::err(RecvError{});
  return xpp::ok(g->m_version.load(std::memory_order_acquire) != m_seen_version);
}

template <class T> Ref<T> Receiver<T>::borrow_and_update() {
  auto g         = m_state->m_value.lock();
  m_seen_version = g->m_version.load(std::memory_order_acquire);
  return Ref<T>(std::move(g));
}

/**
 * @brief Create a new watch channel initialized with the given value.
 *
 * Each send() replaces the stored value and increments a version counter.
 * Receivers independently track which version they've last seen.
 *
 * @tparam T The value type. Must be copy-constructible for the initial value.
 * @param init The initial value placed into the channel.
 * @return A pair of Sender<T> (cloneable) and Receiver<T> (move-only).
 */
template <class T> std::pair<Sender<T>, Receiver<T>> channel(const T &init) {
  auto s = Shared<typename Sender<T>::State>::make(init);
  return {Sender<T>(s), Receiver<T>(std::move(s))};
}

} // namespace watch
} // namespace sync
} // namespace xpp

#endif // XPP_SYNC_WATCH_H
