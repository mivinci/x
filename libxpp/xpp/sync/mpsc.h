/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc.h - xpp::sync::mpsc: bounded & unbounded MPSC channels.
 *
 * Bounded — channel<T>(cap):
 *   Backed by list::channel (lock-free ring buffer). send() may block
 *   when the buffer is full. Coroutine suspension via PromiseResolver.
 *
 * Unbounded — channel<T>():
 *   Backed by list::unbounded_channel (lock-free linked list). send()
 *   never blocks. Aligned with tokio::sync::mpsc.
 *
 * Both variants use RAII close: when the last Sender drops, the channel
 * marks itself closed and wakes any blocked coroutines.
 */

#ifndef XPP_SYNC_MPSC_H
#define XPP_SYNC_MPSC_H

#include <cstddef>
#include <utility>

#include <xpp/loom/internal.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/shared.h>
#include <xpp/sync/list.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace sync {
namespace mpsc {

// ── Error types ──────────────────────────────────────────────────────

/**
 * @brief Error returned by try_send() when the channel is full or closed.
 *
 * The unsent value is preserved in `value` so the caller can retry
 * or handle it without losing ownership.
 */
template <typename T> struct TrySendError {
  enum Kind {
    Full,   ///< Channel buffer is full (bounded only).
    Closed, ///< Channel has been closed; no more values accepted.
  };
  Kind kind;
  T    value;
};

/// Error returned by try_recv() when no value is available.
enum class TryRecvError {
  Empty,  ///< No value currently in the channel.
  Closed, ///< Channel closed and all values consumed.
};

// ── Bounded channel ──────────────────────────────────────────────────

template <class T> class Receiver;

/**
 * @brief Sender for the bounded MPSC channel.
 *
 * Cloneable. Multiple senders can call send() / try_send() concurrently.
 * The data path is lock-free (list::Tx). Coroutine suspension via
 * PromiseResolver when the buffer is full.
 *
 * RAII close: when the last Sender drops, the channel auto-closes.
 *
 * @tparam T Value type. Must be move-constructible.
 *
 * @see Receiver, channel(size_t)
 */
template <class T> class Sender {
public:
  Sender(Sender &&) noexcept = default;
  Sender &operator=(Sender &&) noexcept = default;
  Sender(const Sender &) = default;
  Sender &operator=(const Sender &) = default;
  ~Sender() { drop(); }

  /**
   * @brief Asynchronously send a value. Suspends if full.
   *
   * @param value The value to send. Moved into the channel on success.
   * @return A coroutine promise. Resolves when the value is enqueued
   *         or silently dropped if the channel is closed.
   */
  Promise<void> send(T value) {
    if (!m_chan) co_return;

    while (true) {
      if (m_closed()) co_return;
      if (m_chan->m_tx.try_push(std::move(value))) break;

      auto w = xpp::async<void>();
      m_chan->m_write_waiter = std::move(w.second);
      co_await std::move(w.first);
    }

    if (m_chan->m_read_waiter.is_pending()) {
      auto w = std::move(m_chan->m_read_waiter);
      w.resolve();
    }
  }

  /**
   * @brief Synchronous, non-blocking send.
   *
   * @return Ok if the value was enqueued, or TrySendError on failure.
   */
  Result<Void, TrySendError<T>> try_send(T value) {
    if (!m_chan) return err(TrySendError<T>{TrySendError<T>::Closed, std::move(value)});
    if (m_closed()) return err(TrySendError<T>{TrySendError<T>::Closed, std::move(value)});
    if (!m_chan->m_tx.try_push(std::move(value)))
      return err(TrySendError<T>{TrySendError<T>::Full, std::move(value)});

    if (m_chan->m_read_waiter.is_pending()) {
      auto w = std::move(m_chan->m_read_waiter);
      w.resolve();
    }
    return ok(Void{});
  }

  /**
   * @brief Explicitly close the channel. Idempotent.
   *
   * Wakes any blocked senders and receivers. The receiver can still
   * drain buffered values after close.
   */
  void close() {
    if (!m_chan) return;
    m_chan->m_closed.store(true, std::memory_order_release);

    if (m_chan->m_write_waiter.is_pending()) {
      auto w = std::move(m_chan->m_write_waiter);
      w.resolve();
    }
    if (m_chan->m_read_waiter.is_pending()) {
      auto w = std::move(m_chan->m_read_waiter);
      w.resolve();
    }
  }

private:
  template <class U> friend class Receiver;
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);

  struct Chan {
    list::Tx<T>                   m_tx;
    list::Rx<T>                   m_rx;
    PromiseResolver<void>         m_read_waiter;
    PromiseResolver<void>         m_write_waiter;
    xpp::loom::_::Atomic<size_t>  m_sender_count{1};
    xpp::loom::_::Atomic<bool>    m_closed{false};

    Chan(list::Tx<T> tx, list::Rx<T> rx) : m_tx(std::move(tx)), m_rx(std::move(rx)) {}
  };
  Shared<Chan> m_chan;

  explicit Sender(Shared<Chan> c) : m_chan(std::move(c)) {}

  bool m_closed() const { return m_chan->m_closed.load(std::memory_order_acquire); }

  void drop() {
    if (!m_chan) return;
    if (m_chan->m_sender_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      close();
    }
  }
};

/**
 * @brief Receiver for the bounded MPSC channel. Move-only.
 *
 * Single-consumer: only one thread may call recv() / try_recv().
 *
 * @tparam T Value type.
 *
 * @see Sender, channel(size_t)
 */
template <class T> class Receiver {
public:
  Receiver(Receiver &&) noexcept = default;
  Receiver &operator=(Receiver &&) noexcept = default;

  /**
   * @brief Asynchronously receive the next value. Suspends if empty.
   *
   * @return `some(T)` if a value was received, `none` if the channel
   *         is closed and empty.
   */
  Promise<Option<T>> recv() {
    if (!m_chan) co_return none;

    while (true) {
      auto v = m_chan->m_rx.try_pop();
      if (v.is_some()) {
        if (m_chan->m_write_waiter.is_pending()) {
          auto w = std::move(m_chan->m_write_waiter);
          w.resolve();
        }
        co_return xpp::some(std::move(v).unwrap());
      }

      if (m_closed() && m_chan->m_rx.empty()) co_return none;

      auto pr = xpp::async<void>();
      m_chan->m_read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }
  }

  /**
   * @brief Synchronous, non-blocking receive.
   *
   * @return Ok(T) if a value is available, or TryRecvError.
   */
  Result<T, TryRecvError> try_recv() {
    if (!m_chan) return err(TryRecvError::Closed);

    auto v = m_chan->m_rx.try_pop();
    if (v.is_some()) {
      if (m_chan->m_write_waiter.is_pending()) {
        auto w = std::move(m_chan->m_write_waiter);
        w.resolve();
      }
      return ok(std::move(v).unwrap());
    }
    return err(m_closed() ? TryRecvError::Closed : TryRecvError::Empty);
  }

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  using Chan    = typename Sender<T>::Chan;
  Shared<Chan> m_chan;
  explicit Receiver(Shared<Chan> c) : m_chan(std::move(c)) {}

  bool m_closed() const { return m_chan->m_closed.load(std::memory_order_acquire); }
};

/**
 * @brief Create a bounded MPSC channel.
 *
 * Pre-allocates `cap` slots. send() may block when the buffer is full.
 *
 * @param cap Maximum capacity.
 * @return A pair of Sender<T> (cloneable) and Receiver<T> (move-only).
 *
 * @code
 * auto [tx, rx] = mpsc::channel<int>(16);
 * co_await tx.send(42);
 * auto v = co_await rx.recv();
 * @endcode
 */
template <class T> std::pair<Sender<T>, Receiver<T>> channel(size_t cap) {
  auto [tx, rx] = list::channel<T>(cap);
  auto chan      = Shared<typename Sender<T>::Chan>::make(std::move(tx), std::move(rx));
  return {Sender<T>(chan), Receiver<T>(std::move(chan))};
}

// ── Unbounded channel ────────────────────────────────────────────────

template <class T> class UnboundedReceiver;

/**
 * @brief Sender for the unbounded MPSC channel.
 *
 * Cloneable. send() never blocks — backed by a lock-free linked list
 * (list::UnboundedTx). Each send heap-allocates a Node; each recv
 * deletes it.
 *
 * RAII close when the last Sender drops.
 *
 * @tparam T Value type. Must be move-constructible.
 *
 * @see UnboundedReceiver, channel<T>()
 */
template <class T> class UnboundedSender {
public:
  UnboundedSender(UnboundedSender &&) noexcept = default;
  UnboundedSender &operator=(UnboundedSender &&) noexcept = default;
  UnboundedSender(const UnboundedSender &) = default;
  UnboundedSender &operator=(const UnboundedSender &) = default;
  ~UnboundedSender() { drop(); }

  /**
   * @brief Send a value. Always succeeds — never blocks.
   *
   * @param value The value to send (moved into the channel).
   */
  void send(T value) {
    if (!m_chan) return;
    m_chan->m_tx.push(std::move(value));
    if (m_chan->m_read_waiter.is_pending()) {
      auto w = std::move(m_chan->m_read_waiter);
      w.resolve();
    }
  }

  /**
   * @brief Synchronous, non-blocking send. Always succeeds.
   * @return true (unbounded send never fails due to capacity).
   */
  bool try_send(T value) {
    if (!m_chan) return false;
    m_chan->m_tx.push(std::move(value));
    return true;
  }

  /** @brief Explicitly close the channel. Idempotent. */
  void close() {
    if (!m_chan) return;
    m_chan->m_closed.store(true, std::memory_order_release);
    if (m_chan->m_read_waiter.is_pending()) {
      auto w = std::move(m_chan->m_read_waiter);
      w.resolve();
    }
  }

private:
  template <class U> friend class UnboundedReceiver;
  template <class U> friend std::pair<UnboundedSender<U>, UnboundedReceiver<U>> channel();
  struct Chan {
    list::UnboundedTx<T>          m_tx;
    list::UnboundedRx<T>          m_rx;
    PromiseResolver<void>         m_read_waiter;
    PromiseResolver<void>         m_write_waiter; // unused (unbounded never blocks on full)
    xpp::loom::_::Atomic<size_t>  m_sender_count{1};
    xpp::loom::_::Atomic<bool>    m_closed{false};
    Chan(list::UnboundedTx<T> tx, list::UnboundedRx<T> rx)
        : m_tx(std::move(tx)), m_rx(std::move(rx)) {}
  };
  Shared<Chan> m_chan;
  explicit UnboundedSender(Shared<Chan> c) : m_chan(std::move(c)) {}
  void drop() { if(!m_chan)return; if(m_chan->m_sender_count.fetch_sub(1)==1)close(); }
};

/**
 * @brief Receiver for the unbounded MPSC channel. Move-only.
 *
 * Single-consumer. recv() may block when empty.
 *
 * @tparam T Value type.
 *
 * @see UnboundedSender, channel<T>()
 */
template <class T> class UnboundedReceiver {
public:
  UnboundedReceiver(UnboundedReceiver &&) noexcept = default;
  UnboundedReceiver &operator=(UnboundedReceiver &&) noexcept = default;

  /**
   * @brief Asynchronously receive the next value. Suspends if empty.
   */
  Promise<Option<T>> recv() {
    if (!m_chan) co_return none;

    while (true) {
      auto v = m_chan->m_rx.try_pop();
      if (v.is_some()) {
        if (m_chan->m_write_waiter.is_pending()) {
          auto w = std::move(m_chan->m_write_waiter);
          w.resolve();
        }
        co_return xpp::some(std::move(v).unwrap());
      }

      if (m_closed() && m_chan->m_rx.empty()) co_return none;

      auto pr = xpp::async<void>();
      m_chan->m_read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }
  }

  /** @brief Synchronous, non-blocking receive. */
  Option<T> try_recv() { return m_chan ? m_chan->m_rx.try_pop() : xpp::none; }

private:
  template <class U> friend std::pair<UnboundedSender<U>, UnboundedReceiver<U>> channel();
  using Chan    = typename UnboundedSender<T>::Chan;
  Shared<Chan> m_chan;
  explicit UnboundedReceiver(Shared<Chan> c) : m_chan(std::move(c)) {}
  bool m_closed() const { return m_chan->m_closed.load(std::memory_order_acquire); }
};

/**
 * @brief Create an unbounded MPSC channel.
 *
 * Backed by a lock-free linked list. send() never blocks on full.
 * No capacity limit — limited only by available memory.
 *
 * @return A pair of UnboundedSender<T> (cloneable) and
 *         UnboundedReceiver<T> (move-only).
 */
template <class T> std::pair<UnboundedSender<T>, UnboundedReceiver<T>> channel() {
  auto [tx, rx] = list::unbounded_channel<T>();
  auto chan = Shared<typename UnboundedSender<T>::Chan>::make(std::move(tx), std::move(rx));
  return {UnboundedSender<T>(chan), UnboundedReceiver<T>(std::move(chan))};
}

} // namespace mpsc
} // namespace sync
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_SYNC_MPSC_H
