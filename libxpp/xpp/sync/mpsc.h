/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc.h - xpp::sync::mpsc: bounded multi-producer, single-consumer channel.
 *
 * Backed by xpp::sync::list (lock-free ring buffer). The send path is
 * lock-free (fetch_add). Coroutine suspension via PromiseResolver on
 * empty/full conditions.
 *
 * RAII close when the last Sender drops.
 */

#ifndef XPP_SYNC_MPSC_H
#define XPP_SYNC_MPSC_H

#include <cstddef>
#include <deque>
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

template <typename T> struct TrySendError {
  enum Kind { Full, Closed };
  Kind kind;
  T    value;
};

enum class TryRecvError { Empty, Closed };

template <class T> class Receiver;

template <class T> class Sender {
public:
  Sender(Sender &&) noexcept = default;
  Sender &operator=(Sender &&) noexcept = default;
  Sender(const Sender &) = default;
  Sender &operator=(const Sender &) = default;
  ~Sender() { drop(); }

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
    list::Tx<T>               m_tx;
    list::Rx<T>               m_rx;
    PromiseResolver<void>     m_read_waiter;
    PromiseResolver<void>     m_write_waiter;
    xpp::loom::_::Atomic<size_t> m_sender_count{1};
    xpp::loom::_::Atomic<bool>   m_closed{false};

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

template <class T> class Receiver {
public:
  Receiver(Receiver &&) noexcept = default;
  Receiver &operator=(Receiver &&) noexcept = default;

  Promise<Option<T>> recv() {
    if (!m_chan) co_return none;

    while (true) {
      auto v = m_chan->m_rx.try_pop();
      if (v.is_some()) co_return xpp::some(std::move(v).unwrap());

      if (m_closed() && m_chan->m_rx.empty()) co_return none;

      auto pr = xpp::async<void>();
      m_chan->m_read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }
  }

  Result<T, TryRecvError> try_recv() {
    if (!m_chan) return err(TryRecvError::Closed);

    auto v = m_chan->m_rx.try_pop();
    if (v.is_some()) return ok(std::move(v).unwrap());

    return err(m_closed() ? TryRecvError::Closed : TryRecvError::Empty);
  }

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  using Chan    = typename Sender<T>::Chan;
  Shared<Chan> m_chan;
  explicit Receiver(Shared<Chan> c) : m_chan(std::move(c)) {}

  bool m_closed() const { return m_chan->m_closed.load(std::memory_order_acquire); }
};

template <class T> std::pair<Sender<T>, Receiver<T>> channel(size_t cap) {
  auto [tx, rx] = list::channel<T>(cap);
  auto chan      = Shared<typename Sender<T>::Chan>::make(std::move(tx), std::move(rx));
  return {Sender<T>(chan), Receiver<T>(std::move(chan))};
}

// ── Unbounded channel ───────────────────────────────────────────────

template <class T> class ReceiverUnbounded;

template <class T> class SenderUnbounded {
public:
  SenderUnbounded(SenderUnbounded &&) noexcept = default;
  SenderUnbounded &operator=(SenderUnbounded &&) noexcept = default;
  SenderUnbounded(const SenderUnbounded &) = default;
  SenderUnbounded &operator=(const SenderUnbounded &) = default;
  ~SenderUnbounded() { drop(); }

  // Send never blocks — always succeeds (unbounded).
  void send(T value) {
    if (!m_chan) return;
    m_chan->m_tx.push(std::move(value));
    if (m_chan->m_read_waiter.is_pending()) {
      auto w = std::move(m_chan->m_read_waiter);
      w.resolve();
    }
  }

  bool try_send(T value) {
    if (!m_chan) return false;
    m_chan->m_tx.push(std::move(value));
    return true;
  }

  void close() {
    if (!m_chan) return;
    m_chan->m_closed.store(true, std::memory_order_release);
    if (m_chan->m_read_waiter.is_pending()) {
      auto w = std::move(m_chan->m_read_waiter);
      w.resolve();
    }
  }

private:
  template <class U> friend class ReceiverUnbounded;
  template <class U> friend std::pair<SenderUnbounded<U>, ReceiverUnbounded<U>> channel();
  struct Chan {
    list::UnboundedTx<T>         m_tx;
    list::UnboundedRx<T>         m_rx;
    PromiseResolver<void>        m_read_waiter;
    xpp::loom::_::Atomic<size_t> m_sender_count{1};
    xpp::loom::_::Atomic<bool>   m_closed{false};
    Chan(list::UnboundedTx<T> tx, list::UnboundedRx<T> rx)
        : m_tx(std::move(tx)), m_rx(std::move(rx)) {}
  };
  Shared<Chan> m_chan;
  explicit SenderUnbounded(Shared<Chan> c) : m_chan(std::move(c)) {}
  void drop() { if(!m_chan)return; if(m_chan->m_sender_count.fetch_sub(1)==1)close(); }
};

template <class T> class ReceiverUnbounded {
public:
  ReceiverUnbounded(ReceiverUnbounded &&) noexcept = default;
  ReceiverUnbounded &operator=(ReceiverUnbounded &&) noexcept = default;

  Promise<Option<T>> recv() {
    if (!m_chan) co_return none;

    while (true) {
      auto v = m_chan->m_rx.try_pop();
      if (v.is_some()) co_return xpp::some(std::move(v).unwrap());

      if (m_closed() && m_chan->m_rx.empty()) co_return none;

      auto pr = xpp::async<void>();
      m_chan->m_read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }
  }

  Option<T> try_recv() { return m_chan ? m_chan->m_rx.try_pop() : xpp::none; }

private:
  template <class U> friend std::pair<SenderUnbounded<U>, ReceiverUnbounded<U>> channel();
  using Chan    = typename SenderUnbounded<T>::Chan;
  Shared<Chan> m_chan;
  explicit ReceiverUnbounded(Shared<Chan> c) : m_chan(std::move(c)) {}
  bool m_closed() const { return m_chan->m_closed.load(std::memory_order_acquire); }
};

template <class T> std::pair<SenderUnbounded<T>, ReceiverUnbounded<T>> channel() {
  auto [tx, rx] = list::unbounded_channel<T>();
  auto chan = Shared<typename SenderUnbounded<T>::Chan>::make(std::move(tx), std::move(rx));
  return {SenderUnbounded<T>(chan), ReceiverUnbounded<T>(std::move(chan))};
}

} // namespace mpsc
} // namespace sync
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_SYNC_MPSC_H
