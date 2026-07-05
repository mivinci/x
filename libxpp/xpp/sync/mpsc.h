/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc.h - xpp::sync::mpsc: bounded multi-producer, single-consumer channel.
 *
 * Default (single-threaded): uses Rc<T>, no-op lock, plain counters.
 * With -DXPP_MT: uses Arc<T>, std::mutex + std::atomic for thread-safety.
 *
 * The send-path lock is the equivalent of tokio's second Channel template
 * parameter — a no-op in single-threaded builds and a real mutex in
 * multi-threaded builds.
 */

#ifndef XPP_SYNC_MPSC_H
#define XPP_SYNC_MPSC_H

#include <cstddef>
#include <cstring>
#include <utility>

#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/shared.h>
#include <xpp/loom/internal.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace sync {
namespace mpsc {
namespace _ {

// ── Channel ──────────────────────────────────────────────────────────

template <class T> struct Channel {
  T           *m_buf;
  size_t       m_cap;
  size_t       m_rpos = 0;
  xpp::loom::_::Atomic<size_t> m_wpos{0};
  xpp::loom::_::Atomic<size_t> m_count{0};
  xpp::loom::_::Atomic<bool>   m_closed{false};
  xpp::loom::_::Atomic<size_t> m_sender_count{1};
  xpp::loom::_::Mutex          m_mutex;
  PromiseResolver<void> m_read_waiter;
  PromiseResolver<void> m_write_waiter;

  explicit Channel(size_t cap) : m_buf(new T[cap]), m_cap(cap) {}
  ~Channel() {
    xpp::loom::_::Lock lock(m_mutex);
    // clang-format off
    while (m_count--) m_buf[m_rpos++].~T();
    // clang-format on
    delete[] m_buf;
  }
};

} // namespace _

// ── Error types ──────────────────────────────────────────────────────

/// Error returned by try_send() when the channel is full or closed.
///
/// On failure, the value is returned in `value` so the caller can retry
/// or handle it without losing ownership.
template <typename T> struct TrySendError {
  enum Kind {
    Full,   ///< Channel buffer is full; try again later.
    Closed, ///< Channel has been closed; no more values can be sent.
  };
  Kind kind;
  T    value;
};

/// Error returned by try_recv() when no value is available.
///
/// @see Receiver::try_recv()
enum class TryRecvError {
  Empty,  ///< No value currently in the channel.
  Closed, ///< Channel is closed and all values have been consumed.
};

template <class T> class Receiver;

/**
 * @brief Multi-producer sender for a bounded MPSC channel.
 *
 * Cloneable via `Shared<Channel>`. Multiple senders can `send()`
 * concurrently. When the last `Sender` is dropped, the channel is
 * automatically closed (RAII close). Use `close()` to close earlier.
 *
 * @tparam T The value type. Must be move-constructible.
 *
 * @see mpsc::channel(), mpsc::Receiver
 */
template <class T> class Sender {
public:
  Sender(Sender &&) noexcept            = default;
  Sender &operator=(Sender &&o) noexcept {
    if (this != &o) {
      drop();
      m_chan = std::move(o.m_chan);
    }
    return *this;
  }
  Sender(const Sender &o) : m_chan(o.m_chan) {
    if (m_chan) m_chan->m_sender_count.fetch_add(1, std::memory_order_relaxed);
  }
  Sender &operator=(const Sender &o) {
    if (this != &o) {
      drop();
      m_chan = o.m_chan;
      if (m_chan) m_chan->m_sender_count.fetch_add(1, std::memory_order_relaxed);
    }
    return *this;
  }

  /// Destructor: decrements the sender count. If this is the last
  /// sender, the channel is automatically closed.
  ~Sender() { drop(); }

  /**
   * @brief Asynchronously send a value into the channel.
   *
   * If the buffer is full, the coroutine suspends until a slot becomes
   * available (receiver consumes a value). When `XPP_MT` is defined,
   * the send path is protected by a mutex, and the coroutine releases
   * the lock while waiting to avoid deadlock.
   *
   * @param value The value to send (moved into the channel).
   * @return A promise that resolves when the value has been enqueued.
   *         If the channel is already closed, the send is silently
   *         dropped and the promise resolves immediately.
   */
  Promise<void> send(T value) {
    auto *ch = m_chan.get();
    if (!ch) co_return;

    xpp::loom::_::Lock lock(ch->m_mutex);
    if (ch->m_closed.load(std::memory_order_acquire)) co_return;

    while (ch->m_count.load(std::memory_order_acquire) >= ch->m_cap) {
      auto w = xpp::async<void>();
      ch->m_write_waiter = std::move(w.second);
      lock.unlock();
      co_await std::move(w.first);
      lock = xpp::loom::_::Lock(ch->m_mutex);
      if (ch->m_closed.load(std::memory_order_acquire)) co_return;
    }

    // placement-new: move value into claimed buffer slot
    new (&ch->m_buf[ch->m_wpos.load(std::memory_order_relaxed)]) T(std::move(value));
    ch->m_wpos.store((ch->m_wpos.load(std::memory_order_relaxed) + 1) % ch->m_cap,
                     std::memory_order_relaxed);
    ch->m_count.fetch_add(1, std::memory_order_release);
    lock.unlock();

    if (ch->m_read_waiter.is_pending()) {
      auto w = std::move(ch->m_read_waiter);
      w.resolve();
    }
  }

  /// Synchronous, non-blocking send. Callable from any thread.
  /// Returns: ok(Void) on success, TrySendError on failure.
  Result<Void, TrySendError<T>> try_send(T value) {
    auto *ch = m_chan.get();
    if (!ch) return err(TrySendError<T>{TrySendError<T>::Closed, std::move(value)});

    xpp::loom::_::Lock lock(ch->m_mutex);
    if (ch->m_closed.load(std::memory_order_acquire))
      return err(TrySendError<T>{TrySendError<T>::Closed, std::move(value)});

    if (ch->m_count.load(std::memory_order_acquire) >= ch->m_cap)
      return err(TrySendError<T>{TrySendError<T>::Full, std::move(value)});

    new (&ch->m_buf[ch->m_wpos.load(std::memory_order_relaxed)]) T(std::move(value));
    ch->m_wpos.store((ch->m_wpos.load(std::memory_order_relaxed) + 1) % ch->m_cap,
                     std::memory_order_relaxed);
    ch->m_count.fetch_add(1, std::memory_order_release);
    lock.unlock();

    if (ch->m_read_waiter.is_pending()) {
      auto w = std::move(ch->m_read_waiter);
      w.resolve();
    }
    return ok(Void{});
  }

  /**
   * @brief Close the channel, preventing further sends.
   *
   * All existing values in the buffer remain consumable. Once drained,
   * subsequent `recv()` calls return `none` and `try_recv()` returns
   * `TryRecvError::Closed`. Any blocked senders and receivers are woken.
   *
   * Idempotent — calling `close()` multiple times is safe.
   */
  void close() {
    auto *ch = m_chan.get();
    if (!ch) return;
    close_channel(ch);
  }

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  Shared<_::Channel<T>>                                       m_chan;
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
    if (ch->m_write_waiter.is_pending()) {
      auto w = std::move(ch->m_write_waiter);
      w.resolve();
    }
    if (ch->m_read_waiter.is_pending()) {
      auto w = std::move(ch->m_read_waiter);
      w.resolve();
    }
  }
};

/**
 * @brief Single-consumer receiver for a bounded MPSC channel.
 *
 * There is only ever one receiver. Move-only — the receiver cannot be
 * cloned (single-consumer guarantee).
 *
 * @tparam T The value type.
 *
 * @see mpsc::channel(), mpsc::Sender
 */
template <class T> class Receiver {
public:
  Receiver(Receiver &&) noexcept            = default;
  Receiver &operator=(Receiver &&) noexcept = default;

  /**
   * @brief Asynchronously receive the next value from the channel.
   *
   * If the buffer is empty and the channel is open, the coroutine
   * suspends until a sender enqueues a value. If the channel is empty
   * and closed, returns `none`.
   *
   * @return `some(T)` if a value was received, `none` if the channel
   *         is closed and empty.
   */
  Promise<Option<T>> recv() {
    auto *ch = m_chan.get();
    if (!ch) co_return none;

    while (ch->m_count.load(std::memory_order_acquire) == 0) {
      if (ch->m_closed.load(std::memory_order_acquire)) co_return none;
      auto pr           = xpp::async<void>();
      ch->m_read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }

    T value = std::move(ch->m_buf[ch->m_rpos]);
    ch->m_buf[ch->m_rpos].~T();
    ch->m_rpos = (ch->m_rpos + 1) % ch->m_cap;
    ch->m_count.fetch_sub(1, std::memory_order_release);

    if (ch->m_write_waiter.is_pending()) {
      auto w = std::move(ch->m_write_waiter);
      w.resolve();
    }
    co_return some(std::move(value));
  }

  /// Synchronous, non-blocking recv. Callable from any thread.
  /// Returns: ok(T) on success, TryRecvError on failure.
  Result<T, TryRecvError> try_recv() {
    auto *ch = m_chan.get();
    if (!ch) return err(TryRecvError::Closed);

    if (ch->m_count.load(std::memory_order_acquire) == 0) {
      return err(ch->m_closed.load(std::memory_order_acquire) ? TryRecvError::Closed
                                                               : TryRecvError::Empty);
    }

    T value = std::move(ch->m_buf[ch->m_rpos]);
    ch->m_buf[ch->m_rpos].~T();
    ch->m_rpos = (ch->m_rpos + 1) % ch->m_cap;
    ch->m_count.fetch_sub(1, std::memory_order_release);

    if (ch->m_write_waiter.is_pending()) {
      auto w = std::move(ch->m_write_waiter);
      w.resolve();
    }
    return ok(std::move(value));
  }

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  Shared<_::Channel<T>>                                       m_chan;
  explicit Receiver(Shared<_::Channel<T>> c) : m_chan(std::move(c)) {}
};

/**
 * @brief Create a new bounded MPSC channel.
 *
 * @tparam T The value type. Must be move-constructible.
 * @param cap The maximum number of values the channel can hold before
 *            `send()` blocks or `try_send()` returns `Full`.
 * @return A pair of `Sender<T>` (cloneable) and `Receiver<T>` (move-only).
 *
 * @code
 * auto [tx, rx] = xpp::sync::mpsc::channel<int>(16);
 * @endcode
 */
template <class T> std::pair<Sender<T>, Receiver<T>> channel(size_t cap) {
  auto ch = Shared<_::Channel<T>>::make(cap);
  auto tx = Sender<T>(ch);
  auto rx = Receiver<T>(std::move(ch));
  return {std::move(tx), std::move(rx)};
}

} // namespace mpsc
} // namespace sync
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_SYNC_MPSC_H
