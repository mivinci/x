/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc.h - xpp::sync::mpsc: bounded multi-producer, single-consumer channel.
 */

#ifndef XPP_SYNC_MPSC_H
#define XPP_SYNC_MPSC_H

#include <cstddef>
#include <cstring>
#include <utility>

#include <xpp/promise.h>
#include <xpp/shared.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace sync {
namespace mpsc {

namespace _ {

template <class T> struct Channel {
  T                    *m_buf;
  size_t                m_cap;
  size_t                m_rpos   = 0;
  size_t                m_wpos   = 0;
  size_t                m_count  = 0;
  bool                  m_closed = false;
  PromiseResolver<void> m_read_waiter;
  PromiseResolver<void> m_write_waiter;

  explicit Channel(size_t cap) : m_buf(new T[cap]), m_cap(cap) {}
  ~Channel() {
    while (m_count--)
      m_buf[m_rpos++].~T();
    delete[] m_buf;
  }
};

} // namespace _

template <class T> class Receiver;

template <class T> class Sender {
public:
  Sender(Sender &&) noexcept            = default;
  Sender &operator=(Sender &&) noexcept = default;
  Sender(const Sender &o) : m_ch(o.m_ch) {}
  Sender &operator=(const Sender &o) {
    m_ch = o.m_ch;
    return *this;
  }

  Promise<void> send(T value) {
    auto *ch = m_ch.get();
    if (!ch || ch->m_closed) co_return;

    while (ch->m_count >= ch->m_cap) {
      auto pr            = xpp::async<void>();
      ch->m_write_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }

    // placement-new: move value into buffer slot
    new (&ch->m_buf[ch->m_wpos]) T(std::move(value));
    ch->m_wpos = (ch->m_wpos + 1) % ch->m_cap;
    ++ch->m_count;

    if (ch->m_read_waiter.is_pending()) {
      auto w = std::move(ch->m_read_waiter);
      w.resolve();
    }
  }

  void close() {
    auto *ch = m_ch.get();
    if (!ch || ch->m_closed) return;
    ch->m_closed = true;
    if (ch->m_read_waiter.is_pending()) {
      auto w = std::move(ch->m_read_waiter);
      w.resolve();
    }
    if (ch->m_write_waiter.is_pending()) {
      auto w = std::move(ch->m_write_waiter);
      w.resolve();
    }
  }

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  Shared<_::Channel<T>>                                       m_ch;
  explicit Sender(Shared<_::Channel<T>> c) : m_ch(std::move(c)) {}
};

template <class T> class Receiver {
public:
  Receiver(Receiver &&) noexcept            = default;
  Receiver &operator=(Receiver &&) noexcept = default;

  Promise<Option<T>> recv() {
    auto *ch = m_ch.get();
    if (!ch) co_return none;

    while (ch->m_count == 0) {
      if (ch->m_closed) co_return none;
      auto pr           = xpp::async<void>();
      ch->m_read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }

    T value = std::move(ch->m_buf[ch->m_rpos]);
    ch->m_buf[ch->m_rpos].~T();
    ch->m_rpos = (ch->m_rpos + 1) % ch->m_cap;
    --ch->m_count;

    if (ch->m_write_waiter.is_pending()) {
      auto w = std::move(ch->m_write_waiter);
      w.resolve();
    }
    co_return some(std::move(value));
  }

private:
  template <class U> friend std::pair<Sender<U>, Receiver<U>> channel(size_t cap);
  Shared<_::Channel<T>>                                       m_ch;
  explicit Receiver(Shared<_::Channel<T>> c) : m_ch(std::move(c)) {}
};

template <class T> std::pair<Sender<T>, Receiver<T>> channel(size_t cap) {
  auto ch = Shared<_::Channel<T>>::make(cap);
  return {Sender<T>(ch), Receiver<T>(std::move(ch))};
}

} // namespace mpsc
} // namespace sync
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_SYNC_MPSC_H
