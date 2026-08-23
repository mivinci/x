/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * list.h - xpp::sync::list: lock-free MPSC queues.
 *
 * Bounded variant (channel(cap)):
 *   Pre-allocated ring buffer. push() is lock-free via fetch_add(CAS).
 *   Values stored inline, zero per-push allocation.
 *
 * Unbounded variant (unbounded_channel()):
 *   Lock-free linked list. push() is lock-free via XCHG on tail.
 *   Each push heap-allocates a Node; each pop deletes it.
 *
 * Both are the data-path backends for xpp::sync::mpsc, which adds
 * coroutine suspension on empty/full conditions.
 */

#ifndef XPP_SYNC_LIST_H
#define XPP_SYNC_LIST_H

#include <cstddef>
#include <utility>

#include <xpp/arc.h>
#include <xpp/loom/internal.h>
#include <xpp/option.h>

namespace xpp {
namespace sync {
namespace list {

// ── Forward declarations ────────────────────────────────────────────

template <class T> class Rx;
template <class T> class UnboundedRx;

// ── Bounded (ring-buffer) Core ──────────────────────────────────────

namespace _ {

/**
 * @brief Internal state for the bounded lock-free ring buffer.
 *
 * Pre-allocates `cap` slots. Each slot has a `ready` flag so the
 * consumer never reads half-written data from a concurrent producer.
 *
 * @tparam T The value type stored in each slot.
 */
template <class T> struct Core {
  struct Slot {
    loom::_::Atomic<bool> ready{false}; ///< Set by producer when write completes.
    T                     data;
    Slot() = default;
  };

  Slot                   *m_buf;      ///< Pre-allocated slot array.
  size_t                  m_cap;      ///< Maximum number of in-flight values.
  loom::_::Atomic<size_t> m_wpos{0};  ///< Next write position (CAS'd by producers).
  loom::_::Atomic<size_t> m_count{0}; ///< Number of values currently in the buffer.

  /** @brief Allocate and default-construct `cap` slots. */
  Core(size_t cap) : m_buf(new Slot[cap]), m_cap(cap) {}
  ~Core() {
    delete[] m_buf;
  }
};

} // namespace _

// ── Bounded Tx (multi-producer sender) ──────────────────────────────

/**
 * @brief Lock-free sender for the bounded MPSC ring buffer.
 *
 * Cloneable via Arc<Core>. Multiple instances can call try_push()
 * concurrently — the underlying fetch_add on m_wpos is lock-free.
 *
 * @tparam T The value type.
 *
 * @see Rx, channel(size_t)
 */
template <class T> class Tx {
public:
  Tx(const Tx &)                = default;
  Tx &operator=(const Tx &)     = default;
  Tx(Tx &&) noexcept            = default;
  Tx &operator=(Tx &&) noexcept = default;

  /**
   * @brief Try to push a value into the buffer.
   *
   * @param value The value to enqueue (moved in on success).
   * @return `true` if the value was enqueued, `false` if the buffer
   *         is full (m_count >= m_cap).
   *
   * Lock-free via fetch_add on m_wpos. A `ready` flag per slot
   * ensures the consumer never observes a partially-written value.
   */
  /**
   * @brief Try to push a value into the buffer.
   *
   * @param value Reference to the value to enqueue. Only moved on success.
   *        If this returns false, the value is untouched and can be retried.
   * @return true if the value was enqueued, false if the buffer is full.
   *
   * Lock-free via fetch_add on m_wpos. A `ready` flag per slot
   * ensures the consumer never observes a partially-written value.
   */
  bool try_push(T &value) {
    // Atomically claim a capacity slot. If the old count was at or above
    // capacity, the buffer is full and we undo the claim.
    auto  *c  = m_core.get();
    size_t c0 = c->m_count.fetch_add(1, std::memory_order_acquire);
    if (c0 >= c->m_cap) {
      c->m_count.fetch_sub(1, std::memory_order_relaxed);
      return false;
    }

    size_t w   = c->m_wpos.fetch_add(1, std::memory_order_acq_rel);
    size_t idx = w % c->m_cap;

    auto &s = c->m_buf[idx];
    s.data  = std::move(value);
    s.ready.store(true, std::memory_order_release);
    return true;
  }

  /** @brief Maximum capacity. */
  size_t capacity() const {
    return m_core->m_cap;
  }

  /** @brief Current number of buffered values. */
  size_t count() const {
    return m_core->m_count.load(std::memory_order_acquire);
  }

private:
  template <class U> friend std::pair<Tx<U>, Rx<U>> channel(size_t cap);
  Arc<_::Core<T>>                                   m_core;
  explicit Tx(Arc<_::Core<T>> c) : m_core(std::move(c)) {}
};

// ── Bounded Rx (single-consumer receiver) ───────────────────────────

/**
 * @brief Single-consumer receiver for the bounded MPSC ring buffer.
 *
 * Not thread-safe — only one thread may call try_pop() at a time.
 * This matches the MPSC contract: multi-producer, *single*-consumer.
 *
 * @tparam T The value type.
 *
 * @see Tx, channel(size_t)
 */
template <class T> class Rx {
public:
  Rx(const Rx &)                = delete;
  Rx &operator=(const Rx &)     = delete;
  Rx(Rx &&) noexcept            = default;
  Rx &operator=(Rx &&) noexcept = default;

  /**
   * @brief Try to pop a value from the buffer.
   *
   * @return The value if available, `none` if the buffer is empty.
   *
   * Spins briefly on the slot's `ready` flag if a producer has claimed
   * the slot but not yet finished writing. The spin is bounded because
   * writes are just a move + store.
   */
  xpp::Option<T> try_pop() {
    auto *c = m_core.get();
    if (c->m_count.load(std::memory_order_acquire) == 0) return xpp::none;

    size_t idx = m_rpos % c->m_cap;
    auto  &s   = c->m_buf[idx];

    // Spin briefly while producer finishes writing the slot.
    // Production writes are just move + store (nanoseconds).
    // The pause hint reduces power on SMT siblings during spin.
    while (!s.ready.load(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
      __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
      __asm__ __volatile__("yield" ::: "memory");
#endif
    }

    T val = std::move(s.data);
    // Replace the moved-from value with a default-constructed T so the slot
    // holds a valid, empty object. Core's destructor will call ~T() again
    // via delete[] — calling ~T() on the moved-from object here would be a
    // double destructor (UB for non-trivially-destructible types like Bytes,
    // whose Option<Arc<Impl>> member would dec a refcount on already-freed
    // memory).
    new (&s.data) T();
    s.ready.store(false, std::memory_order_relaxed);
    ++m_rpos;
    c->m_count.fetch_sub(1, std::memory_order_release);
    return xpp::some(std::move(val));
  }

  /** @brief True if no values are currently buffered. */
  bool empty() const {
    return m_core->m_count.load(std::memory_order_acquire) == 0;
  }

private:
  template <class U> friend std::pair<Tx<U>, Rx<U>> channel(size_t cap);
  Arc<_::Core<T>>                                   m_core;
  size_t                                            m_rpos = 0;
  explicit Rx(Arc<_::Core<T>> c) : m_core(std::move(c)) {}
};

/**
 * @brief Create a bounded lock-free MPSC ring buffer.
 *
 * Pre-allocates `cap` slots. Values are stored inline — no heap
 * allocation per push/pop.
 *
 * @param cap Maximum capacity.
 * @return A pair of Tx<T> (cloneable) and Rx<T> (move-only).
 */
template <class T> std::pair<Tx<T>, Rx<T>> channel(size_t cap) {
  auto core = Arc<_::Core<T>>::make(cap);
  return {Tx<T>(core), Rx<T>(std::move(core))};
}

// ── Unbounded (linked-list) Core ─────────────────────────────────────

/**
 * @brief Internal state for the unbounded lock-free linked list.
 *
 * Each push heap-allocates a Node. Nodes are freed by pop().
 * The list uses the Dmitry Vyukov MPSC algorithm: producers XCHG
 * on the tail pointer, consumer reads and advances head.
 *
 * @tparam T The value type.
 */
template <class T> struct UnboundedCore {
  struct Node {
    Node *volatile next = nullptr; ///< Next node in the list.
    T data;                        ///< Stored value.

    explicit Node(T v) : data(std::move(v)) {}
  };

  loom::_::Atomic<Node *> m_head{nullptr}; ///< First node (consumer reads).
  loom::_::Atomic<Node *> m_tail{nullptr}; ///< Last node (producers append).

  ~UnboundedCore() {
    Node *n = m_head.load(std::memory_order_relaxed);
    while (n) {
      Node *next = n->next;
      delete n;
      n = next;
    }
  }
};

// ── Unbounded Tx (multi-producer sender) ────────────────────────────

/**
 * @brief Lock-free sender for the unbounded MPSC linked list.
 *
 * Cloneable via Arc<Core>. push() always succeeds (heap-allocates
 * a Node per value). There is no capacity limit — push never blocks.
 *
 * @tparam T The value type.
 *
 * @see UnboundedRx, unbounded_channel()
 */
template <class T> class UnboundedTx {
public:
  UnboundedTx(const UnboundedTx &)                = default;
  UnboundedTx &operator=(const UnboundedTx &)     = default;
  UnboundedTx(UnboundedTx &&) noexcept            = default;
  UnboundedTx &operator=(UnboundedTx &&) noexcept = default;

  /**
   * @brief Push a value onto the list.
   *
   * Always succeeds (heap-allocates a Node). Lock-free: uses atomic
   * XCHG on the tail pointer, then links the previous node.
   *
   * @param value The value to enqueue.
   */
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
  Arc<UnboundedCore<T>>                                               m_core;
  explicit UnboundedTx(Arc<UnboundedCore<T>> c) : m_core(std::move(c)) {}
};

// ── Unbounded Rx (single-consumer receiver) ─────────────────────────

/**
 * @brief Single-consumer receiver for the unbounded MPSC linked list.
 *
 * Not thread-safe — only one thread may call try_pop() at a time.
 * Each pop deletes the consumed node.
 *
 * @tparam T The value type.
 *
 * @see UnboundedTx, unbounded_channel()
 */
template <class T> class UnboundedRx {
public:
  UnboundedRx(const UnboundedRx &)                = delete;
  UnboundedRx &operator=(const UnboundedRx &)     = delete;
  UnboundedRx(UnboundedRx &&) noexcept            = default;
  UnboundedRx &operator=(UnboundedRx &&) noexcept = default;

  /**
   * @brief Try to pop a value from the list.
   *
   * @return The value if available, `none` if the list is empty.
   *
   * Uses the Vyukov single-consumer pop: loads head, handles the
   * single-node (head == tail) and multi-node cases with CAS on tail.
   */
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

  /** @brief True if the list has no nodes. */
  bool empty() const {
    return m_core->m_head.load(std::memory_order_acquire) == nullptr;
  }

private:
  template <class U> friend std::pair<UnboundedTx<U>, UnboundedRx<U>> unbounded_channel();
  Arc<UnboundedCore<T>>                                               m_core;
  explicit UnboundedRx(Arc<UnboundedCore<T>> c) : m_core(std::move(c)) {}
};

/**
 * @brief Create an unbounded lock-free MPSC linked list.
 *
 * Each push heap-allocates a Node; each pop deletes it.
 * No capacity limit — push never blocks (limited only by memory).
 *
 * @return A pair of UnboundedTx<T> (cloneable) and UnboundedRx<T> (move-only).
 */
template <class T> std::pair<UnboundedTx<T>, UnboundedRx<T>> unbounded_channel() {
  auto core = Arc<UnboundedCore<T>>::make();
  return {UnboundedTx<T>(core), UnboundedRx<T>(std::move(core))};
}

} // namespace list
} // namespace sync
} // namespace xpp

#endif // XPP_SYNC_LIST_H
