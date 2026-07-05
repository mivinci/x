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
#include <xpp/shared.h>

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
  Tx(const Tx &) = default;
  Tx &operator=(const Tx &) = default;
  Tx(Tx &&) noexcept = default;
  Tx &operator=(Tx &&) noexcept = default;

  bool try_push(T value) {
    auto *c = m_core.get();
    size_t c0 = c->m_count.load(std::memory_order_acquire);
    if (c0 >= c->m_cap) return false;

    size_t w   = c->m_wpos.fetch_add(1, std::memory_order_acq_rel);
    size_t idx = w % c->m_cap;

    auto &s  = c->m_buf[idx];
    s.data   = std::move(value);
    s.ready.store(true, std::memory_order_release);
    c->m_count.fetch_add(1, std::memory_order_release);
    return true;
  }

  size_t capacity() const { return m_core->m_cap; }
  size_t count() const { return m_core->m_count.load(std::memory_order_acquire); }

private:
  template <class U> friend std::pair<Tx<U>, Rx<U>> channel(size_t cap);
  Shared<_::Core<T>> m_core;
  explicit Tx(Shared<_::Core<T>> c) : m_core(std::move(c)) {}
};

template <class T> class Rx {
public:
  Rx(const Rx &) = delete;
  Rx &operator=(const Rx &) = delete;
  Rx(Rx &&) noexcept = default;
  Rx &operator=(Rx &&) noexcept = default;

  xpp::Option<T> try_pop() {
    auto *c = m_core.get();
    if (c->m_count.load(std::memory_order_acquire) == 0)
      return xpp::none;

    size_t idx = m_rpos % c->m_cap;
    auto  &s   = c->m_buf[idx];

    while (!s.ready.load(std::memory_order_acquire)) {}

    T val = std::move(s.data);
    s.data.~T();
    s.ready.store(false, std::memory_order_relaxed);
    ++m_rpos;
    c->m_count.fetch_sub(1, std::memory_order_release);
    return xpp::some(std::move(val));
  }

  bool empty() const { return m_core->m_count.load(std::memory_order_acquire) == 0; }

private:
  template <class U> friend std::pair<Tx<U>, Rx<U>> channel(size_t cap);
  Shared<_::Core<T>> m_core;
  size_t             m_rpos = 0;
  explicit Rx(Shared<_::Core<T>> c) : m_core(std::move(c)) {}
};

template <class T> std::pair<Tx<T>, Rx<T>> channel(size_t cap) {
  auto core = Shared<_::Core<T>>::make(cap);
  return {Tx<T>(core), Rx<T>(std::move(core))};
}

// ── Unbounded (linked-list) Core ─────────────────────────────────────

template <class T> class UnboundedRx;

template <class T> struct UnboundedCore {
  struct Node {
    Node *volatile next = nullptr;
    T      data;

    explicit Node(T v) : data(std::move(v)) {}
  };

  loom::_::Atomic<Node *> m_head{nullptr};
  loom::_::Atomic<Node *> m_tail{nullptr};

  ~UnboundedCore() {
    Node *n = m_head.load(std::memory_order_relaxed);
    while (n) {
      Node *next = n->next;
      delete n;
      n = next;
    }
  }
};

template <class T> class UnboundedTx {
public:
  UnboundedTx(const UnboundedTx &) = default;
  UnboundedTx &operator=(const UnboundedTx &) = default;
  UnboundedTx(UnboundedTx &&) noexcept = default;
  UnboundedTx &operator=(UnboundedTx &&) noexcept = default;

  void push(T value) {
    auto *node = new typename UnboundedCore<T>::Node(std::move(value));
    node->next = nullptr;

    auto *old_tail = m_core->m_tail.exchange(node, std::memory_order_acq_rel);
    if (old_tail) {
      old_tail->next = node;
    } else {
      m_core->m_head.store(node, std::memory_order_release);
    }
  }

private:
  template <class U> friend std::pair<UnboundedTx<U>, UnboundedRx<U>> unbounded_channel();
  Shared<UnboundedCore<T>> m_core;
  explicit UnboundedTx(Shared<UnboundedCore<T>> c) : m_core(std::move(c)) {}
};

template <class T> class UnboundedRx {
public:
  UnboundedRx(const UnboundedRx &) = delete;
  UnboundedRx &operator=(const UnboundedRx &) = delete;
  UnboundedRx(UnboundedRx &&) noexcept = default;
  UnboundedRx &operator=(UnboundedRx &&) noexcept = default;

  xpp::Option<T> try_pop() {
    auto *head = m_core->m_head.load(std::memory_order_acquire);
    if (!head) return xpp::none;

    auto *next = head->next;
    if (!next) {
      auto *expected = head;
      if (!m_core->m_tail.compare_exchange_strong(expected, nullptr, std::memory_order_release)) {
        while (!(next = head->next)) {}
      }
      m_core->m_head.store(next, std::memory_order_release);
    } else {
      m_core->m_head.store(next, std::memory_order_release);
    }

    T val = std::move(head->data);
    delete head;
    return xpp::some(std::move(val));
  }

  bool empty() const { return m_core->m_head.load(std::memory_order_acquire) == nullptr; }

private:
  template <class U> friend std::pair<UnboundedTx<U>, UnboundedRx<U>> unbounded_channel();
  Shared<UnboundedCore<T>> m_core;
  explicit UnboundedRx(Shared<UnboundedCore<T>> c) : m_core(std::move(c)) {}
};

template <class T> std::pair<UnboundedTx<T>, UnboundedRx<T>> unbounded_channel() {
  auto core = Shared<UnboundedCore<T>>::make();
  return {UnboundedTx<T>(core), UnboundedRx<T>(std::move(core))};
}

} // namespace list
} // namespace sync
} // namespace xpp

#endif
