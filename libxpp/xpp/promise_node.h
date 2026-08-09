/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_node.h - Internal PromiseNode<T> hierarchy for xpp::Promise<T>.
 *
 * This header defines the virtual PromiseNode interface and the concrete
 * node types that implement Promise chaining, flattening, and deferred
 * resolution. Users should not include this directly — use <xpp/promise.h>.
 *
 * C++11-compatible. Header-only (templates).
 */

#ifndef XPP_PROMISE_NODE_H
#define XPP_PROMISE_NODE_H

#include <utility>

#include <xpp/compiler.h>
#include <xpp/event.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/promise_allocator.h>
#include <xpp/promise_context.h>
#include <xpp/void.h>

namespace xpp {

template <class T> class Promise;

namespace _ {

/* ── ReturnType helper ──────────────���────────────────────────────── */

template <class Func, class Arg>
using ReturnType = decltype(std::declval<Func>()(std::declval<Arg>()));

template <class Func> using ReturnTypeVoid = decltype(std::declval<Func>()());

/* ── ReducePromise ───────────────────────────────────────────────── */

template <class T> struct ReducePromise {
  using Type = T;
};
template <class U> struct ReducePromise<Promise<U>> {
  using Type = U;
};

/* ── PromiseNode<T> ──────────────────────────────────────────────── */

/**
 * @brief Core async computation interface — analogous to Rust's `Future` trait.
 *
 * A `PromiseNode` represents an asynchronous operation that may not have
 * completed yet.  The operation is driven by repeated calls to `poll()`
 * until it signals completion.  This is a pull-based model: the caller
 * (typically `Promise::await()`) asks "are you done yet?", and the node
 * either hands over the result or says "not yet — wake me up when
 * something changes."
 *
 * @par The poll() contract
 *
 * Each `poll()` call returns an `Option<ValueType>`:
 *
 * - `Some(value)` — the operation is complete.  The value is extracted
 *   in the same call; no separate `take()` step.  **After returning
 *   `Some`, `poll()` must never be called again** — the node is
 *   considered consumed and may be destroyed.
 *
 * - `None` — the operation is still pending.  Before returning `None`,
 *   the node **must** arrange to be woken up when progress is possible.
 *   It does this by extracting the waker from `cx.waker()` and storing
 *   it (cloning the `PromiseWaker` as needed).  When the underlying
 *   operation completes, the stored waker's `wake()` is called, which
 *   causes the awaiter to re-poll this node.
 *
 * @par The waker protocol
 *
 * `poll()` receives a `const PromiseContext &cx`.  `PromiseContext` is
 * non-cloneable — it only lives for the duration of the `poll()` call.
 * To wake the awaiter later, extract the `PromiseWaker` and store a copy:
 *
 * @code
 * Option<T> poll(const PromiseContext &cx) override {
 *   if (m_ready) return Some(std::move(m_value));
 *
 *   // Store a clone of the waker so we can fire it later.
 *   m_atomic_waker.register_by_ref(cx.waker());
 *
 *   // Re-check after registering — the operation may have completed
 *   // in the race window between the first check and register.
 *   if (m_ready) {
 *     m_atomic_waker.wake();   // self-wake: avoid lost wakeup
 *     return Some(std::move(m_value));
 *   }
 *   return none;               // still pending — awaiter will park
 * }
 * @endcode
 *
 * The double-check after `register_by_ref` is critical: without it, a
 * completion that races with registration would be lost (the waker was
 * stored too late, and `wake()` was already called before registration).
 *
 * @par One-shot semantics
 *
 * A `PromiseNode` is single-use.  Once `poll()` returns `Some`, the node
 * is consumed.  There is no way to "reset" it.  If you need to poll the
 * same logical operation again, create a new node.
 *
 * @par Thread safety
 *
 * - `poll()` is called from the WaitScope thread (the thread that owns
 *   the event loop).  It is not thread-safe — do not call `poll()` from
 *   multiple threads simultaneously.
 * - However, the *resolution* of the underlying operation may happen on
 *   a different thread (e.g., a thread-pool worker calling
 *   `PromiseResolver::resolve()`).  That path is thread-safe: it uses
 *   `AtomicPromiseWaker` + `std::atomic` to coordinate with `poll()`.
 *
 * @par The void specialization
 *
 * `PromiseNode<void>` uses `Void` as the value type and returns
 * `Option<Void>`.  `Some(Void{})` signals completion; there is no
 * payload to carry.
 *
 * @see PromiseContext  — the poll context (non-cloneable, owns the waker)
 * @see PromiseWaker    — the cloneable wake handle (extract via cx.waker())
 * @see AtomicPromiseWaker — lock-free cell for storing a waker across threads
 */
template <class T> class PromiseNode {
public:
  using ValueType = typename FixVoid<T>::Type;

  virtual ~PromiseNode() = default;

  /**
   * @brief Check if the operation is complete.
   *
   * @param cx  The poll context.  Extract `cx.waker()` and clone it if
   *            you need to wake the awaiter later.  The context is only
   *            valid for the duration of this call — do not store it.
   * @return `Some(value)` if complete (node is now consumed);
   *         `None` if still pending (waker must have been registered).
   */
  virtual Option<ValueType> poll(const PromiseContext &cx) = 0;
};

/**
 * @brief void specialization — same contract as PromiseNode<T>, but the
 *        result carries no value (`Option<Void>`).
 */
template <> class PromiseNode<void> {
public:
  using ValueType = Void;

  virtual ~PromiseNode() = default;

  virtual Option<Void> poll(const PromiseContext &cx) = 0;
};

/* ── Convenience typedef: arena-aware Own for PromiseNode ─────────── */
template <class T> using OwnPromiseNode = Own<PromiseNode<T>, _::PromiseNodeAllocator>;

/* ── ImmediatePromiseNode ────────────────────────────────────── */

template <class T> class ImmediatePromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  explicit ImmediatePromiseNode(T &&value) : m_val(std::move(value)) {}

  Option<ValueType> poll(const PromiseContext &) override {
    return Option<ValueType>(std::move(m_val));
  }

private:
  ValueType m_val;
};

template <> class ImmediatePromiseNode<void> final : public PromiseNode<void> {
public:
  Option<Void> poll(const PromiseContext &) override {
    return Option<Void>(Void{});
  }
};

/* ── TransformPromiseNode ────────────────────────────────────── */

template <class U, class T, class Func> class TransformPromiseNode final : public PromiseNode<U> {
public:
  using OutputType = typename PromiseNode<U>::ValueType;

  TransformPromiseNode(_::OwnPromiseNode<T> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<OutputType> poll(const PromiseContext &cx) override {
    auto r = m_dep->poll(cx);
    if (r.is_none()) return none;
    return Option<OutputType>(::xpp::_voidwrap::call1<U>(m_fn, std::move(r).unwrap()));
  }

private:
  _::OwnPromiseNode<T> m_dep;
  Func                 m_fn;
};

template <class U, class Func>
class TransformPromiseNode<U, void, Func> final : public PromiseNode<U> {
public:
  using OutputType = typename PromiseNode<U>::ValueType;

  TransformPromiseNode(_::OwnPromiseNode<void> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<OutputType> poll(const PromiseContext &cx) override {
    auto r = m_dep->poll(cx);
    if (r.is_none()) return none;
    std::move(r).unwrap(); // consume the Void
    return Option<OutputType>(::xpp::_voidwrap::call<U>(m_fn));
  }

private:
  _::OwnPromiseNode<void> m_dep;
  Func                    m_fn;
};

template <class T, class Func>
class TransformPromiseNode<void, T, Func> final : public PromiseNode<void> {
public:
  TransformPromiseNode(_::OwnPromiseNode<T> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<Void> poll(const PromiseContext &cx) override {
    auto r = m_dep->poll(cx);
    if (r.is_none()) return none;
    ::xpp::_voidwrap::call1<void>(m_fn, std::move(r).unwrap());
    return Option<Void>(Void{});
  }

private:
  _::OwnPromiseNode<T> m_dep;
  Func                 m_fn;
};

template <class Func>
class TransformPromiseNode<void, void, Func> final : public PromiseNode<void> {
public:
  TransformPromiseNode(_::OwnPromiseNode<void> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<Void> poll(const PromiseContext &cx) override {
    auto r = m_dep->poll(cx);
    if (r.is_none()) return none;
    std::move(r).unwrap();
    ::xpp::_voidwrap::call<void>(m_fn);
    return Option<Void>(Void{});
  }

private:
  _::OwnPromiseNode<void> m_dep;
  Func                    m_fn;
};

/* ── ChainPromiseNode ────────────────────────────────────────────── */

/**
 * @brief Flattens Promise<Promise<T>> into Promise<T>.
 *
 * Polls the outer node. When the outer returns Some(Promise<T>),
 * extracts the inner node and polls it.
 *
 * Uses m_inner != nullptr instead of an enum state machine:
 *   m_inner == nullptr → still polling outer
 *   m_inner != nullptr → outer done, polling inner
 */
template <class T> class ChainPromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  explicit ChainPromiseNode(_::OwnPromiseNode<Promise<T>> outer) : m_outer(std::move(outer)) {}

  Option<ValueType> poll(const PromiseContext &cx) override {
    if (m_inner) {
      return m_inner->poll(cx);
    }
    auto outer = m_outer->poll(cx);
    if (outer.is_none()) return none;
    m_inner = std::move(std::move(outer).unwrap().m_node);
    m_outer = nullptr;
    return m_inner->poll(cx);
  }

private:
  _::OwnPromiseNode<Promise<T>> m_outer;
  _::OwnPromiseNode<T>          m_inner;
};

/* ── YieldPromiseNode ────────────────────────────────────────────── */

class YieldPromiseNode final : public PromiseNode<void> {
public:
  Option<Void> poll(const PromiseContext &) override {
    return Option<Void>(Void{});
  }
};

} // namespace _
} // namespace xpp

#endif // XPP_PROMISE_NODE_H
