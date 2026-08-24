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
 *
 * ── Waiter protocol (tokio-aligned) ─────────────────────────────────
 *
 * Receivers park via a level-triggered poll node (RecvPromiseNode):
 * try_pop → empty → register the waker in the channel's
 * AtomicPromiseWaker slot → try_pop again → Pending. Every successful
 * push wakes the slot. The AtomicWaker state machine plus the
 * post-registration re-check make the register/push race
 * lost-wakeup-free (same structure as tokio's chan.rs). Late/duplicate
 * wakes are harmless: the spawn driver tolerates them.
 *
 * Senders (bounded, buffer full) park in a mutex-guarded FIFO of poll
 * nodes; the push attempt and the FIFO registration happen in one
 * critical section, so a concurrent pop cannot free capacity without
 * waking exactly one sender. The mutex only appears on the cold path
 * (buffer full); the data path stays lock-free.
 */

#ifndef XPP_SYNC_MPSC_H
#define XPP_SYNC_MPSC_H

#include <cstddef>
#include <utility>

#include <xpp/arc.h>
#include <xpp/loom/internal.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/sync/list.h>

namespace xpp {
namespace sync {
namespace mpsc {

// ── Error types (C++11, no coroutine dependency) ─────────────────────

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

namespace _ {

template <class T> class SendPromiseNode; // fwd — parks in Chan's writer FIFO

/**
 * @brief Shared state of a bounded channel.
 *
 * Waiter protocol — see the file header. m_rx_waker is the receiver
 * wake slot (lock-free, register→re-check→wake). m_tx_head/m_tx_tail
 * form the intrusive FIFO of suspended senders, entirely guarded by
 * m_tx_mtx; the fast paths (successful try_push / try_pop) never take
 * the mutex — it only appears on the backpressure path (buffer full),
 * where the sender is about to suspend anyway.
 */
template <class T> struct Chan {
  typedef T ValueType;

  list::Tx<T> m_tx;
  list::Rx<T> m_rx;

  /// Parked-receiver wake slot (see AtomicPromiseWaker).
  AtomicPromiseWaker m_rx_waker;

  /// Suspended senders (buffer full): intrusive doubly-linked FIFO.
  /// All guarded by m_tx_mtx.
  loom::_::Mutex               m_tx_mtx;
  SendPromiseNode<T>          *m_tx_head = nullptr;
  SendPromiseNode<T>          *m_tx_tail = nullptr;
  xpp::loom::_::Atomic<size_t> m_tx_waiter_count{0};

  xpp::loom::_::Atomic<size_t> m_sender_count{1};
  xpp::loom::_::Atomic<bool>   m_closed{false};

  Chan(list::Tx<T> tx, list::Rx<T> rx) : m_tx(std::move(tx)), m_rx(std::move(rx)) {}

  bool closed() const {
    return m_closed.load(std::memory_order_acquire);
  }

  /// Caller holds m_tx_mtx. Appends to the FIFO tail.
  void enqueue_writer_locked(SendPromiseNode<T> *w) {
    w->m_prev = m_tx_tail;
    w->m_next = nullptr;
    if (m_tx_tail)
      m_tx_tail->m_next = w;
    else
      m_tx_head = w;
    m_tx_tail = w;
    m_tx_waiter_count.fetch_add(1, std::memory_order_release);
  }

  /// Caller holds m_tx_mtx. Detaches the front waiter (FIFO — no starvation).
  SendPromiseNode<T> *take_writer_locked() {
    SendPromiseNode<T> *w = m_tx_head;
    if (!w) return nullptr;
    m_tx_head = w->m_next;
    if (m_tx_head)
      m_tx_head->m_prev = nullptr;
    else
      m_tx_tail = nullptr;
    w->m_next = w->m_prev = nullptr;
    w->m_registered       = false;
    m_tx_waiter_count.fetch_sub(1, std::memory_order_release);
    return w;
  }

  /// A slot freed up (a value was popped): wake one suspended sender.
  /// Lock-free when nobody is parked.
  void wake_one_writer() {
    if (m_tx_waiter_count.load(std::memory_order_acquire) == 0) return;
    SendPromiseNode<T> *w = nullptr;
    {
      loom::_::Lock g(m_tx_mtx);
      w = take_writer_locked();
    }
    if (w) w->m_waker.unwrap().wake();
  }

  /// Channel closed: wake every suspended sender.
  void wake_all_writers() {
    SendPromiseNode<T> *head = nullptr;
    {
      loom::_::Lock g(m_tx_mtx);
      head      = m_tx_head;
      m_tx_head = m_tx_tail = nullptr;
      SendPromiseNode<T> *w = head;
      while (w) {
        w->m_registered = false;
        m_tx_waiter_count.fetch_sub(1, std::memory_order_release);
        w = w->m_next;
      }
    }
    /* The detached chain is still linked via m_next; fire outside the
     * lock. Cancellation (node destruction) can no longer race — every
     * node was marked unregistered under the lock. */
    while (head) {
      SendPromiseNode<T> *next = head->m_next;
      head->m_prev = head->m_next = nullptr;
      head->m_waker.unwrap().wake();
      head = next;
    }
  }
};

/**
 * @brief recv() node — level-triggered poll over the shared queue.
 *
 * try_pop; empty → register the waker; try_pop again (closes the
 * lost-wakeup window against a concurrent push); Pending. The stale
 * registration left behind when the re-check succeeds is harmless —
 * the next register overwrites it and late wakes are no-ops.
 */
template <class ChanT>
class RecvPromiseNode final : public xpp::_::PromiseNode<Option<typename ChanT::ValueType>> {
public:
  typedef Option<typename ChanT::ValueType> ValueType;
  typedef typename ChanT::ValueType         T;

  explicit RecvPromiseNode(Arc<ChanT> chan) : m_chan(std::move(chan)) {}

  Option<ValueType> poll(const PromiseContext &cx) override {
    auto v = m_chan->m_rx.try_pop();
    if (v.is_none() && m_chan->closed() && m_chan->m_rx.empty()) {
      m_chan->m_rx_waker.take_waker();           // may hold our waker from the previous poll
      return Option<ValueType>(Option<T>(none)); // closed and drained
    }
    if (v.is_some()) {
      m_chan->m_rx_waker.take_waker(); // ditto (woken between polls)
      m_chan->wake_one_writer();       // a slot freed up
      return v;
    }

    /* Empty and open. Register, then re-check — the AtomicWaker
     * protocol plus this re-check make the race with a concurrent
     * push lost-wakeup-free. */
    m_chan->m_rx_waker.register_by_ref(cx.waker());
    v = m_chan->m_rx.try_pop();
    if (v.is_some()) {
      m_chan->m_rx_waker.take_waker(); // clear the stale registration
      m_chan->wake_one_writer();
      return v;
    }
    if (m_chan->closed() && m_chan->m_rx.empty()) {
      m_chan->m_rx_waker.take_waker();
      return Option<ValueType>(Option<T>(none));
    }
    return none;
  }

private:
  Arc<ChanT> m_chan;
};

/**
 * @brief send() node — push, or park in the writer FIFO when full.
 *
 * Fast path (space available): a lock-free try_push, no mutex. Slow
 * path (full): under the channel mutex, re-attempt the push and, if
 * still full, register in the FIFO — one critical section, so a
 * concurrent pop that frees capacity necessarily dequeues and wakes
 * this node.
 */
template <class T> class SendPromiseNode final : public xpp::_::PromiseNode<void> {
public:
  SendPromiseNode(Arc<Chan<T>> chan, T value)
      : m_chan(std::move(chan)), m_value(std::move(value)) {}

  ~SendPromiseNode() {
    if (m_registered) {
      loom::_::Lock g(m_chan->m_tx_mtx);
      unregister_locked();
    }
  }

  Option<Void> poll(const PromiseContext &cx) override {
    if (m_done) return Option<Void>(Void{});

    /* Fast path — space available: lock-free push, no mutex. */
    if (!m_chan->closed() && m_chan->m_tx.try_push(m_value)) {
      m_done = true;
      m_chan->m_rx_waker.wake(); // data available — wake a parked receiver
      return Option<Void>(Void{});
    }

    bool pushed = false;
    {
      loom::_::Lock g(m_chan->m_tx_mtx);
      if (m_chan->closed()) {
        m_done = true; // closed: value silently dropped (send() semantics)
      } else if (m_chan->m_tx.try_push(m_value)) {
        m_done = true;
        pushed = true;
      } else if (!m_registered) {
        m_chan->enqueue_writer_locked(this);
        m_registered = true;
        m_waker      = cx.waker();
      }
      /* else: already registered — still full, keep waiting. */
    }
    if (m_done) {
      if (pushed) m_chan->m_rx_waker.wake();
      return Option<Void>(Void{});
    }
    return none;
  }

private:
  friend struct Chan<T>;

  /// Caller holds m_chan->m_tx_mtx. O(1) unlink.
  void unregister_locked() {
    if (m_prev)
      m_prev->m_next = m_next;
    else
      m_chan->m_tx_head = m_next;
    if (m_next)
      m_next->m_prev = m_prev;
    else
      m_chan->m_tx_tail = m_prev;
    m_prev = m_next = nullptr;
    m_registered    = false;
    m_chan->m_tx_waiter_count.fetch_sub(1, std::memory_order_release);
  }

  Arc<Chan<T>>         m_chan;
  T                    m_value;
  Option<PromiseWaker> m_waker;                // guarded by m_chan->m_tx_mtx
  SendPromiseNode<T>  *m_prev       = nullptr; // FIFO links, guarded by m_chan->m_tx_mtx
  SendPromiseNode<T>  *m_next       = nullptr;
  bool                 m_registered = false; // in the FIFO (guarded)
  bool                 m_done       = false;
};

} // namespace _

template <class T> class Receiver;

/**
 * @brief Sender for the bounded MPSC channel.
 *
 * Cloneable. Multiple senders can call send() / try_send() concurrently.
 * The data path is lock-free (list::Tx); coroutine suspension via
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
  Sender(Sender &&) noexcept            = default;
  Sender &operator=(Sender &&) noexcept = default;
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
  ~Sender() {
    drop();
  }

  /**
   * @brief Asynchronously send a value. Suspends if full.
   *
   * @param value The value to send. Moved into the channel on success.
   * @return A promise. Resolves when the value is enqueued or silently
   *         dropped if the channel is closed.
   */
  Promise<void> send(T value);

  /**
   * @brief Synchronous, non-blocking send.
   *
   * @return Ok if the value was enqueued, or TrySendError on failure.
   */
  Result<Void, TrySendError<T>> try_send(T value);

  /**
   * @brief Explicitly close the channel. Idempotent.
   *
   * Wakes any blocked senders and receivers. The receiver can still
   * drain buffered values after close.
   */
  void close();

private:
  template <class U> friend class Receiver;
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);

  Arc<_::Chan<T>> m_chan;
  explicit Sender(Arc<_::Chan<T>> c) : m_chan(std::move(c)) {}

  void drop() {
    if (!m_chan) return;
    if (m_chan->m_sender_count.fetch_sub(1, std::memory_order_acq_rel) == 1) close();
  }
};

/**
 * @brief Receiver for the bounded MPSC channel. Move-only.
 *
 * Single-consumer: only one thread may call recv() / try_recv().
 * This matches the MPSC contract: multi-producer, *single*-consumer.
 *
 * @tparam T Value type.
 *
 * @see Sender, channel(size_t)
 */
template <class T> class Receiver {
public:
  Receiver(Receiver &&) noexcept            = default;
  Receiver &operator=(Receiver &&) noexcept = default;

  /**
   * @brief Asynchronously receive the next value. Suspends if empty.
   *
   * @return `some(T)` if a value was received, `none` if the channel
   *         is closed and empty.
   */
  Promise<Option<T>> recv();

  /**
   * @brief Synchronous, non-blocking receive.
   *
   * @return Ok(T) if a value is available, or TryRecvError.
   */
  Result<T, TryRecvError> try_recv();

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  Arc<_::Chan<T>>                                             m_chan;
  explicit Receiver(Arc<_::Chan<T>> c) : m_chan(std::move(c)) {}
};

/* ── send() / recv() — one implementation for C++11 and C++20 ────── */

template <class T> Promise<void> Sender<T>::send(T value) {
  if (!m_chan) return xpp::resolve();
  return Promise<void>(xpp::_::OwnPromiseNode<void>(
    xpp::_::promise::allocate<_::SendPromiseNode<T>>(nullptr, m_chan, std::move(value))));
}

template <class T> Promise<Option<T>> Receiver<T>::recv() {
  if (!m_chan) return xpp::resolve(Option<T>(none));
  return Promise<Option<T>>(xpp::_::OwnPromiseNode<Option<T>>(
    xpp::_::promise::allocate<_::RecvPromiseNode<_::Chan<T>>>(nullptr, m_chan)));
}

/* ── Synchronous methods ───────────────────────────────────────────── */

template <class T> Result<Void, TrySendError<T>> Sender<T>::try_send(T value) {
  if (!m_chan) return err(TrySendError<T>{TrySendError<T>::Closed, std::move(value)});
  if (m_chan->closed()) return err(TrySendError<T>{TrySendError<T>::Closed, std::move(value)});
  if (!m_chan->m_tx.try_push(value))
    return err(TrySendError<T>{TrySendError<T>::Full, std::move(value)});

  m_chan->m_rx_waker.wake(); // a parked receiver may now make progress
  return ok(Void{});
}

template <class T> void Sender<T>::close() {
  if (!m_chan) return;
  m_chan->m_closed.store(true, std::memory_order_release);

  m_chan->wake_all_writers(); // suspended senders: fail fast
  m_chan->m_rx_waker.wake();  // parked receiver: observe close
}

template <class T> Result<T, TryRecvError> Receiver<T>::try_recv() {
  if (!m_chan) return err(TryRecvError::Closed);

  auto v = m_chan->m_rx.try_pop();
  if (v.is_some()) {
    m_chan->wake_one_writer(); // a slot freed up (no-op when nobody parks)
    return ok(std::move(v).unwrap());
  }
  return err(m_chan->closed() && m_chan->m_rx.empty() ? TryRecvError::Closed : TryRecvError::Empty);
}

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
  auto chan     = Arc<_::Chan<T>>::make(std::move(tx), std::move(rx));
  return {Sender<T>(chan), Receiver<T>(std::move(chan))};
}

// ── Unbounded channel ────────────────────────────────────────────────

namespace _ {

/**
 * @brief Shared state of an unbounded channel.
 *
 * send() never blocks (no capacity limit), so there are no suspended
 * senders — only the receiver wake slot. wake_*_writer() are no-op
 * stubs so RecvPromiseNode works unchanged for both channel kinds.
 */
template <class T> struct UnboundedChan {
  typedef T ValueType;

  list::UnboundedTx<T> m_tx;
  list::UnboundedRx<T> m_rx;

  AtomicPromiseWaker m_rx_waker;

  xpp::loom::_::Atomic<size_t> m_sender_count{1};
  xpp::loom::_::Atomic<bool>   m_closed{false};

  UnboundedChan(list::UnboundedTx<T> tx, list::UnboundedRx<T> rx)
      : m_tx(std::move(tx)), m_rx(std::move(rx)) {}

  bool closed() const {
    return m_closed.load(std::memory_order_acquire);
  }

  void wake_one_writer() {} // senders never park (unbounded)
  void wake_all_writers() {}
};

} // namespace _

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
  UnboundedSender(UnboundedSender &&) noexcept            = default;
  UnboundedSender &operator=(UnboundedSender &&) noexcept = default;
  UnboundedSender(const UnboundedSender &)                = default;
  UnboundedSender &operator=(const UnboundedSender &)     = default;
  ~UnboundedSender() {
    drop();
  }

  /**
   * @brief Send a value. Always succeeds — never blocks.
   *
   * @param value The value to send (moved into the channel).
   */
  void send(T value);

  /**
   * @brief Synchronous, non-blocking send. Always succeeds.
   * @return true (unbounded send never fails due to capacity).
   */
  bool try_send(T value);

  /** @brief Explicitly close the channel. Idempotent. */
  void close();

private:
  template <class U> friend class UnboundedReceiver;
  template <class U> friend std::pair<UnboundedSender<U>, UnboundedReceiver<U>> channel();

  Arc<_::UnboundedChan<T>> m_chan;
  explicit UnboundedSender(Arc<_::UnboundedChan<T>> c) : m_chan(std::move(c)) {}
  void drop() {
    if (!m_chan) return;
    if (m_chan->m_sender_count.fetch_sub(1, std::memory_order_acq_rel) == 1) close();
  }
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
  UnboundedReceiver(UnboundedReceiver &&) noexcept            = default;
  UnboundedReceiver &operator=(UnboundedReceiver &&) noexcept = default;

  /**
   * @brief Asynchronously receive the next value. Suspends if empty.
   */
  Promise<Option<T>> recv();

  /** @brief Synchronous, non-blocking receive. */
  Option<T> try_recv() {
    return m_chan ? m_chan->m_rx.try_pop() : xpp::none;
  }

private:
  template <class U> friend std::pair<UnboundedSender<U>, UnboundedReceiver<U>> channel();
  Arc<_::UnboundedChan<T>>                                                      m_chan;
  explicit UnboundedReceiver(Arc<_::UnboundedChan<T>> c) : m_chan(std::move(c)) {}
};

/* ── UnboundedReceiver::recv() body ────────────────────────────────── */

template <class T> Promise<Option<T>> UnboundedReceiver<T>::recv() {
  if (!m_chan) return xpp::resolve(Option<T>(none));
  return Promise<Option<T>>(xpp::_::OwnPromiseNode<Option<T>>(
    xpp::_::promise::allocate<_::RecvPromiseNode<_::UnboundedChan<T>>>(nullptr, m_chan)));
}

/* ── UnboundedSender synchronous methods ──────────────────────────── */

template <class T> void UnboundedSender<T>::send(T value) {
  if (!m_chan) return;
  m_chan->m_tx.push(std::move(value));
  m_chan->m_rx_waker.wake(); // a parked receiver may now make progress
}

template <class T> bool UnboundedSender<T>::try_send(T value) {
  if (!m_chan) return false;
  m_chan->m_tx.push(std::move(value));
  m_chan->m_rx_waker.wake();
  return true;
}

template <class T> void UnboundedSender<T>::close() {
  if (!m_chan) return;
  m_chan->m_closed.store(true, std::memory_order_release);
  m_chan->m_rx_waker.wake(); // parked receiver: observe close
}

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
  auto chan     = Arc<_::UnboundedChan<T>>::make(std::move(tx), std::move(rx));
  return {UnboundedSender<T>(chan), UnboundedReceiver<T>(std::move(chan))};
}

} // namespace mpsc
} // namespace sync
} // namespace xpp

#endif // XPP_SYNC_MPSC_H
