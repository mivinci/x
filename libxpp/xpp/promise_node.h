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
 * C++17-compatible. Header-only (templates).
 */

#ifndef XPP_PROMISE_NODE_H
#define XPP_PROMISE_NODE_H

#include <utility>

#include <xpp/compiler.h>
#include <xpp/event.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/promise_waker.h>
#include <xpp/void.h>

namespace xpp {

template <class T> class Promise;

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

namespace _ {

/* ── PromiseNode<T> ──────────────────────────────────────────────── */

/**
 * @brief Core async computation interface, analogous to Rust's Future trait.
 *
 * poll(const PromiseWaker &) → Option<ValueType>:
 *   Some(value) = ready, value extracted in the same call
 *   None        = pending, waker stored for later notification
 *
 * One-shot: once poll returns Some, it must never be called again.
 *
 * There is no separate take() — readiness and value are atomic.
 */
template <class T> class PromiseNode {
public:
  using ValueType = typename FixVoid<T>::Type;

  virtual ~PromiseNode() = default;

  virtual Option<ValueType> poll(const PromiseWaker &waker) = 0;
};

template <> class PromiseNode<void> {
public:
  using ValueType = Void;

  virtual ~PromiseNode() = default;

  virtual Option<Void> poll(const PromiseWaker &waker) = 0;
};

/* ── ImmediatePromiseNode ────────────────────────────────────── */

template <class T> class ImmediatePromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  explicit ImmediatePromiseNode(T &&value) : m_val(std::move(value)) {}

  Option<ValueType> poll(const PromiseWaker &) override {
    return Option<ValueType>(std::move(m_val));
  }

private:
  ValueType m_val;
};

template <> class ImmediatePromiseNode<void> final : public PromiseNode<void> {
public:
  Option<Void> poll(const PromiseWaker &) override {
    return Option<Void>(Void{});
  }
};

/* ── TransformPromiseNode ────────────────────────────────────── */

template <class U, class T, class Func> class TransformPromiseNode final : public PromiseNode<U> {
public:
  using OutputType = typename PromiseNode<U>::ValueType;

  TransformPromiseNode(Own<PromiseNode<T>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<OutputType> poll(const PromiseWaker &waker) override {
    auto r = m_dep->poll(waker);
    if (r.is_none()) return none;
    return Option<OutputType>(::xpp::_voidwrap::call1<U>(m_fn, r.unwrap()));
  }

private:
  Own<PromiseNode<T>> m_dep;
  Func                m_fn;
};

template <class U, class Func>
class TransformPromiseNode<U, void, Func> final : public PromiseNode<U> {
public:
  using OutputType = typename PromiseNode<U>::ValueType;

  TransformPromiseNode(Own<PromiseNode<void>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<OutputType> poll(const PromiseWaker &waker) override {
    auto r = m_dep->poll(waker);
    if (r.is_none()) return none;
    r.unwrap(); // consume the Void
    return Option<OutputType>(::xpp::_voidwrap::call<U>(m_fn));
  }

private:
  Own<PromiseNode<void>> m_dep;
  Func                   m_fn;
};

template <class T, class Func>
class TransformPromiseNode<void, T, Func> final : public PromiseNode<void> {
public:
  TransformPromiseNode(Own<PromiseNode<T>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<Void> poll(const PromiseWaker &waker) override {
    auto r = m_dep->poll(waker);
    if (r.is_none()) return none;
    ::xpp::_voidwrap::call1<void>(m_fn, r.unwrap());
    return Option<Void>(Void{});
  }

private:
  Own<PromiseNode<T>> m_dep;
  Func                m_fn;
};

template <class Func>
class TransformPromiseNode<void, void, Func> final : public PromiseNode<void> {
public:
  TransformPromiseNode(Own<PromiseNode<void>> dep, Func &&func)
      : m_dep(std::move(dep)), m_fn(std::move(func)) {}

  Option<Void> poll(const PromiseWaker &waker) override {
    auto r = m_dep->poll(waker);
    if (r.is_none()) return none;
    r.unwrap();
    ::xpp::_voidwrap::call<void>(m_fn);
    return Option<Void>(Void{});
  }

private:
  Own<PromiseNode<void>> m_dep;
  Func                   m_fn;
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

  explicit ChainPromiseNode(Own<PromiseNode<Promise<T>>> outer) : m_outer(std::move(outer)) {}

  Option<ValueType> poll(const PromiseWaker &waker) override {
    if (m_inner) {
      return m_inner->poll(waker);
    }
    auto outer = m_outer->poll(waker);
    if (outer.is_none()) return none;
    m_inner = std::move(outer.unwrap().m_node);
    m_outer = nullptr;
    return m_inner->poll(waker);
  }

private:
  Own<PromiseNode<Promise<T>>> m_outer;
  Own<PromiseNode<T>>          m_inner;
};

/* ── AdapterPromiseNode ──────────────────────────────────────── */

/**
 * @brief Bridges external (synchronous) resolution into the Promise system.
 *
 * The caller obtains a Promise backed by this node, then hands out the
 * adapter pointer to code that will eventually call resolve(). The
 * executor side polls the node via poll(); when resolve() fires, the
 * stored waker is woken and the next poll() returns Some.
 */
template <class T> class AdapterPromiseNode final : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  AdapterPromiseNode() {}

  Option<ValueType> poll(const PromiseWaker &waker) override {
    if (m_resolved.load(std::memory_order_acquire)) {
      return std::move(m_val);
    }
    m_waker.register_waker(std::move(waker));
    if (m_resolved.load(std::memory_order_acquire)) {
      m_waker.wake();
      return std::move(m_val);
    }
    return none;
  }

  void resolve(T &&value) {
    XPP_ASSERT(!m_resolved.load(std::memory_order_relaxed), "AdapterPromiseNode resolved twice");
    m_val = Option<ValueType>(std::move(value));
    m_resolved.store(true, std::memory_order_release);
    m_waker.wake();
  }

private:
  Option<ValueType>  m_val;
  PromiseAtomicWaker m_waker;
  std::atomic<bool>  m_resolved{false};
};

/// AdapterPromiseNode<Void> — same protocol, no value storage.
template <> class AdapterPromiseNode<Void> final : public PromiseNode<void> {
public:
  AdapterPromiseNode() {}

  Option<Void> poll(const PromiseWaker &waker) override {
    if (m_resolved.load(std::memory_order_acquire)) {
      return Option<Void>(Void{});
    }
    m_waker.register_waker(std::move(waker));
    if (m_resolved.load(std::memory_order_acquire)) {
      m_waker.wake();
      return Option<Void>(Void{});
    }
    return none;
  }

  void resolve() {
    XPP_ASSERT(!m_resolved.load(std::memory_order_relaxed), "AdapterPromiseNode resolved twice");
    m_resolved.store(true, std::memory_order_release);
    m_waker.wake();
  }

private:
  PromiseAtomicWaker m_waker;
  std::atomic<bool>  m_resolved{false};
};

/* ── YieldPromiseNode ────────────────────────────────────────────── */

class YieldPromiseNode final : public PromiseNode<void> {
public:
  Option<Void> poll(const PromiseWaker &) override {
    return Option<Void>(Void{});
  }
};

} // namespace _
} // namespace xpp

#endif // XPP_PROMISE_NODE_H
