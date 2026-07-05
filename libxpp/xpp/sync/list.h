/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * list.h - xpp::sync::list: lock-free bounded MPSC ring buffer.
 *
 * Stores values inline. send() is lock-free (fetch_add). recv() is SP.
 */

#ifndef XPP_SYNC_LIST_H
#define XPP_SYNC_LIST_H

#include <cstddef>
#include <utility>

#include <xpp/loom/internal.h>
#include <xpp/option.h>

namespace xpp {
namespace sync {
namespace list {

template <class T> class Rx;

namespace _ {

template <class T> struct Core {
  struct Slot {
    loom::_::Atomic<bool> ready{false};
    T                     data;
    Slot() = default;
  };

  Slot                   *m_buf;
  size_t                  m_cap;
  loom::_::Atomic<size_t> m_wpos{0};
  loom::_::Atomic<size_t> m_count{0};

  Core(size_t cap) : m_buf(new Slot[cap]), m_cap(cap) {}
  ~Core() { delete[] m_buf; }
};

} // namespace _

template <class T> class Tx {
public:
  Tx(const Tx &) = delete;
  Tx &operator=(const Tx &) = delete;
  Tx(Tx &&) noexcept = default;
  Tx &operator=(Tx &&) noexcept = default;

  bool try_push(T value) {
    size_t c = m_core->m_count.load(std::memory_order_acquire);
    if (c >= m_core->m_cap) return false;

    size_t w   = m_core->m_wpos.fetch_add(1, std::memory_order_acq_rel);
    size_t idx = w % m_core->m_cap;

    auto &s  = m_core->m_buf[idx];
    s.data   = std::move(value);
    s.ready.store(true, std::memory_order_release);
    m_core->m_count.fetch_add(1, std::memory_order_release);
    return true;
  }

  size_t capacity() const { return m_core->m_cap; }

private:
  template <class U> friend std::pair<Tx<U>, Rx<U>> channel(size_t cap);
  _::Core<T> *m_core;
  explicit Tx(_::Core<T> *c) : m_core(c) {}
};

template <class T> class Rx {
public:
  Rx(const Rx &) = delete;
  Rx &operator=(const Rx &) = delete;
  Rx(Rx &&) noexcept = default;
  Rx &operator=(Rx &&) noexcept = default;

  xpp::Option<T> try_pop() {
    if (m_core->m_count.load(std::memory_order_acquire) == 0)
      return xpp::none;

    size_t idx = m_rpos % m_core->m_cap;
    auto  &s   = m_core->m_buf[idx];

    // Spin until sender finishes writing into this slot.
    // In practice the write is just a move+store, so the spin
    // should be extremely short (microseconds at worst).
    while (!s.ready.load(std::memory_order_acquire)) {}

    T val = std::move(s.data);
    s.data.~T();
    s.ready.store(false, std::memory_order_relaxed);
    m_rpos++;
    m_core->m_count.fetch_sub(1, std::memory_order_release);
    return xpp::some(std::move(val));
  }

private:
  template <class U> friend std::pair<Tx<U>, Rx<U>> channel(size_t cap);
  _::Core<T> *m_core;
  size_t      m_rpos = 0;
  explicit Rx(_::Core<T> *c) : m_core(c) {}
};

template <class T> std::pair<Tx<T>, Rx<T>> channel(size_t cap) {
  auto *core = new _::Core<T>(cap);
  return {Tx<T>(core), Rx<T>(core)};
}

} // namespace list
} // namespace sync
} // namespace xpp

#endif
