/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * broadcast.h - xpp::sync::broadcast: multi-producer, multi-consumer channel.
 *
 * Every sent value is seen by all connected receivers. The buffer is
 * bounded — when full, the oldest value is evicted. Receivers that fall
 * behind get RecvError::Lagged(n) and auto-reset to the current head.
 *
 * RAII close via last Sender drop. Aligns with tokio::sync::broadcast.
 */

#ifndef XPP_SYNC_BROADCAST_H
#define XPP_SYNC_BROADCAST_H

#include <utility>

#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/shared.h>
#include <xpp/loom/internal.h>
#include <xpp/sync/notify.h>

namespace xpp {
namespace sync {
namespace broadcast {

namespace _ {

// ── Channel ──────────────────────────────────────────────────────────

template <class T> struct Channel {
  T                         *m_buf;
  size_t                     m_cap;
  size_t                     m_head              = 0;
  size_t                     m_tail              = 0;
  xpp::loom::_::Atomic<size_t>    m_sender_count{1};
  xpp::loom::_::Atomic<bool>      m_closed{false};
  xpp::loom::_::Mutex             m_mutex;
  xpp::sync::Notify          m_notify;

  // Count of active receivers (used by send() return value).
  xpp::loom::_::Atomic<size_t>    m_receiver_count{1};

  explicit Channel(size_t cap) : m_buf(new T[cap]), m_cap(cap) {}
  ~Channel() {
    xpp::loom::_::Lock lock(m_mutex);
    // Destroy any remaining values in the buffer.
    while (m_head != m_tail) {
      m_buf[m_head].~T();
      m_head = (m_head + 1) % m_cap;
    }
    delete[] m_buf;
  }
};

} // namespace _

// ── Error types ──────────────────────────────────────────────────────

/**
 * @brief Error returned by Sender::send() and Sender::try_send().
 *
 * Contains the original value on failure so the caller can decide
 * what to do with it.
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

/**
 * @brief Error returned by Receiver::recv().
 *
 * Indicates that the receiver could not get a value — either because
 * it lagged behind (sender evicted unread values) or the channel is
 * closed and empty.
 */
enum class RecvError {
  Lagged, ///< Receiver missed values; read position auto-resets to head.
  Closed, ///< All senders dropped and buffer is empty.
};

/**
 * @brief Error returned by Receiver::try_recv().
 *
 * A non-blocking variant of RecvError. Includes an Empty variant
 * for the case where a value is not yet available.
 */
enum class TryRecvError {
  Empty,  ///< No value available yet; try again later.
  Closed, ///< All senders dropped and buffer is empty.
  Lagged, ///< Receiver missed values; read position auto-resets to head.
};

template <class T> class Receiver;

/**
 * @brief Multi-producer sender for a broadcast channel.
 *
 * Cloneable. Every call to send() delivers the value to all connected
 * receivers. Never blocks — when full, the oldest value is evicted.
 * RAII close: when the last Sender is dropped, the channel closes.
 *
 * @see broadcast::channel(), broadcast::Receiver
 */
template <class T> class Sender {
public:
  Sender(Sender &&) noexcept = default;
  Sender &operator=(Sender &&o) noexcept {
    if (this != &o) {
      drop();
      m_chan = std::move(o.m_chan);
    }
    return *this;
  }
  Sender(const Sender &o) : m_chan(o.m_chan) {
    if (m_chan) m_chan->m_sender_count.fetch_add(1);
  }
  Sender &operator=(const Sender &o) {
    if (this != &o) {
      drop();
      m_chan = o.m_chan;
      if (m_chan) m_chan->m_sender_count.fetch_add(1);
    }
    return *this;
  }
  ~Sender() { drop(); }

  /**
   * @brief Send a value to all receivers. Never blocks.
   *
   * If the buffer is full, the oldest value is evicted. Receivers
   * that haven't read the evicted value get RecvError::Lagged.
   *
   * @return Ok(n) where n is the number of active receivers, or
   *         Err(SendError::NoReceiver) if no receivers.
   */
  xpp::Promise<xpp::Result<size_t, SendError<T>>> send(T value) {
    auto *ch = m_chan.get();
    if (!ch) co_return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

    xpp::loom::_::Lock lock(ch->m_mutex);

    if (ch->m_closed.load(std::memory_order_acquire))
      co_return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

    size_t n = ch->m_receiver_count.load(std::memory_order_acquire);
    if (n == 0)
      co_return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

    size_t next = ch->m_tail;
    // If full, evict oldest and advance head.
    if ((next + 1) % ch->m_cap == ch->m_head) {
      ch->m_buf[ch->m_head].~T();
      ch->m_head = (ch->m_head + 1) % ch->m_cap;
    }

    new (&ch->m_buf[next]) T(std::move(value));
    ch->m_tail = (next + 1) % ch->m_cap;
    lock.unlock();

    ch->m_notify.notify_waiters();
    co_return xpp::ok(n);
  }

  /**
   * @brief Synchronous, non-blocking send.
   */
  xpp::Result<size_t, SendError<T>> try_send(T value) {
    auto *ch = m_chan.get();
    if (!ch) return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

    xpp::loom::_::Lock lock(ch->m_mutex);

    if (ch->m_closed.load(std::memory_order_acquire))
      return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

    size_t n = ch->m_receiver_count.load(std::memory_order_acquire);
    if (n == 0)
      return xpp::err(SendError<T>{SendError<T>::NoReceiver, std::move(value)});

    size_t next = ch->m_tail;
    if ((next + 1) % ch->m_cap == ch->m_head) {
      ch->m_buf[ch->m_head].~T();
      ch->m_head = (ch->m_head + 1) % ch->m_cap;
    }

    new (&ch->m_buf[next]) T(std::move(value));
    ch->m_tail = (next + 1) % ch->m_cap;
    lock.unlock();

    ch->m_notify.notify_waiters();
    return xpp::ok(n);
  }

  /**
   * @brief Create a new Receiver that sees only values sent after this call.
   */
  Receiver<T> subscribe() {
    auto *ch = m_chan.get();
    ch->m_receiver_count.fetch_add(1, std::memory_order_relaxed);
    return Receiver<T>(m_chan, ch->m_tail);
  }

  /// Number of buffered values (head-to-tail inclusive).
  size_t len() const {
    if (!m_chan || m_chan->m_head == m_chan->m_tail) return 0;
    return (m_chan->m_tail + m_chan->m_cap - m_chan->m_head) % m_chan->m_cap;
  }

  /// Number of active receivers.
  size_t receiver_count() const {
    return m_chan ? m_chan->m_receiver_count.load(std::memory_order_relaxed) : 0;
  }

  /// Explicitly close the channel.
  void close() {
    auto *ch = m_chan.get();
    if (!ch) return;
    close_channel(ch);
  }

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  template <class U> friend class Receiver;
  Shared<_::Channel<T>> m_chan;

  explicit Sender(Shared<_::Channel<T>> c) : m_chan(std::move(c)) {}

  void drop() {
    if (!m_chan) return;
    if (m_chan->m_sender_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      close_channel(m_chan.get());
    }
  }

  void close_channel(_::Channel<T> *ch) {
    {
      xpp::loom::_::Lock lock(ch->m_mutex);
      if (ch->m_closed.load(std::memory_order_acquire)) return;
      ch->m_closed.store(true, std::memory_order_release);
    }
    ch->m_notify.notify_waiters();
  }
};

/**
 * @brief Receiver for a broadcast channel. Move-only.
 *
 * Each receiver tracks its own read position. If the sender evicts
 * values this receiver hasn't read yet, recv() returns Lagged.
 */
template <class T> class Receiver {
public:
  Receiver(Receiver &&) noexcept = default;
  Receiver &operator=(Receiver &&) noexcept = default;

  ~Receiver() {
    if (!m_chan) return;
    m_chan->m_receiver_count.fetch_sub(1, std::memory_order_relaxed);
  }

  /**
   * @brief Await the next value from the channel.
   *
   * Suspends the coroutine until a value is available or the channel
   * is closed. If the receiver has fallen behind (sender evicted values
   * this receiver hasn't read yet), returns Lagged and auto-resets to
   * the current head.
   *
   * @return Ok(T) with the next value, or Err(RecvError) on failure.
   */
  xpp::Promise<xpp::Result<T, RecvError>> recv() {
    auto *ch = m_chan.get();
    if (!ch) co_return xpp::err(RecvError::Closed);

    // Wait until data is available or channel is closed.
    while (m_pos == ch->m_tail) {
      if (ch->m_closed.load(std::memory_order_acquire))
        co_return xpp::err(RecvError::Closed);
      co_await ch->m_notify.notified();
    }

    // Check for lag: m_pos should be in [m_head, m_tail) going forward.
    // Lag occurs when m_head overtakes m_pos (sender evicted values).
    bool lagged;
    if (ch->m_head <= ch->m_tail) {
      lagged = (m_pos < ch->m_head || m_pos >= ch->m_tail);
    } else {
      lagged = (m_pos < ch->m_head && m_pos >= ch->m_tail);
    }
    if (lagged) {
      m_pos = ch->m_head;
      co_return xpp::err(RecvError::Lagged);
    }

    T value = std::move(ch->m_buf[m_pos]);
    m_pos   = (m_pos + 1) % ch->m_cap;
    co_return xpp::ok(std::move(value));
  }

  /// Synchronous, non-blocking recv.
  xpp::Result<T, TryRecvError> try_recv() {
    auto *ch = m_chan.get();
    if (!ch) return xpp::err(TryRecvError::Closed);

    if (m_pos == ch->m_tail) {
      return xpp::err(ch->m_closed.load(std::memory_order_acquire) ? TryRecvError::Closed
                                                                    : TryRecvError::Empty);
    }

    bool lagged;
    if (ch->m_head <= ch->m_tail) {
      lagged = (m_pos < ch->m_head || m_pos >= ch->m_tail);
    } else {
      lagged = (m_pos < ch->m_head && m_pos >= ch->m_tail);
    }
    if (lagged) {
      m_pos = ch->m_head;
      return xpp::err(TryRecvError::Lagged);
    }

    T value = std::move(ch->m_buf[m_pos]);
    m_pos   = (m_pos + 1) % ch->m_cap;
    return xpp::ok(std::move(value));
  }

private:
  template <class U> friend class Sender;
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  Shared<_::Channel<T>> m_chan;
  size_t                m_pos;

  Receiver(Shared<_::Channel<T>> c, size_t pos) : m_chan(std::move(c)), m_pos(pos) {}
};

/**
 * @brief Create a bounded broadcast channel.
 *
 * @param cap Maximum number of values the buffer can retain.
 * @return A pair of Sender<T> (cloneable) and Receiver<T> (move-only).
 */
template <class T> std::pair<Sender<T>, Receiver<T>> channel(size_t cap) {
  auto ch = Shared<_::Channel<T>>::make(cap);
  auto tx = Sender<T>(ch);
  auto rx = Receiver<T>(std::move(ch), 0);
  return {std::move(tx), std::move(rx)};
}

} // namespace broadcast
} // namespace sync
} // namespace xpp

#endif // XPP_SYNC_BROADCAST_H
