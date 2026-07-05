/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter.h - Core adapter infrastructure for Promise.
 *
 * ResolveState<T> + PromiseResolver<T> + AdapterPromiseNode<T, Adapter>.
 * PromiseResolver holds ArcWeak — resolve() is safe after Promise destruction.
 *
 * Specific adapters (TimerAdapter, WorkAdapter) are in separate headers.
 *
 * C++11-compatible. Header-only.
 */
#ifndef XPP_PROMISE_ADAPTER_H
#define XPP_PROMISE_ADAPTER_H

#include <atomic>
#include <utility>

#include <xpp/arc.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/promise_node.h>
#include <xpp/promise_waker.h>
#include <xpp/void.h>

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

template <class T> class PromiseResolver;

namespace _ {

/* ── ResolveState<T> ─────────────────────────────────────────────── */

template <class T> struct ResolveState {
  Option<T>          value;
  AtomicPromiseWaker waker;
  std::atomic<bool>  resolved{false};
};

/* ── poll_state helper (shared by all node types) ────────────────── */

template <class T> inline Option<T> poll_state(ResolveState<T> &s, const PromiseWaker &waker) {
  if (s.resolved.load(std::memory_order_acquire)) {
    return std::move(s.value);
  }
  s.waker.register_waker(waker);
  if (s.resolved.load(std::memory_order_acquire)) {
    s.waker.wake();
    return std::move(s.value);
  }
  return none;
}

inline Option<Void> poll_state(ResolveState<Void> &s, const PromiseWaker &waker) {
  if (s.resolved.load(std::memory_order_acquire)) {
    return Option<Void>(Void{});
  }
  s.waker.register_waker(waker);
  if (s.resolved.load(std::memory_order_acquire)) {
    s.waker.wake();
    return Option<Void>(Void{});
  }
  return none;
}

/* ── ManualResolveNode<T> ────────────────────────────────────────── */

template <class T> class ManualResolveNode : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;
  explicit ManualResolveNode(Arc<ResolveState<ValueType>> state) : m_state(std::move(state)) {}
  Option<ValueType> poll(const PromiseWaker &waker) override {
    return poll_state(*m_state, waker);
  }

private:
  Arc<ResolveState<ValueType>> m_state;
};

/* ── AdapterPromiseNode<T, Adapter> ──────────────────────────────── */

template <class T, class Adapter> class AdapterPromiseNode : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  template <class... AdapterArgs>
  explicit AdapterPromiseNode(AdapterArgs &&...args)
      : m_state(Arc<ResolveState<ValueType>>::make()),
        m_adapter(PromiseResolver<T>(Arc<ResolveState<ValueType>>::downgrade(m_state)),
                  std::forward<AdapterArgs>(args)...) {}

  Option<ValueType> poll(const PromiseWaker &waker) override {
    return poll_state(*m_state, waker);
  }

private:
  Arc<ResolveState<ValueType>> m_state;
  Adapter                      m_adapter;
};

} // namespace _

/* ── PromiseResolver<T> ─────────────────────────────────────────────── */

template <class T> class PromiseResolver {
public:
  using ValueType = typename FixVoid<T>::Type;

  PromiseResolver()                                       = default;
  PromiseResolver(PromiseResolver &&) noexcept            = default;
  PromiseResolver &operator=(PromiseResolver &&) noexcept = default;
  PromiseResolver(const PromiseResolver &)                = delete;
  PromiseResolver &operator=(const PromiseResolver &)     = delete;

  void resolve(ValueType &&value) {
    auto s = m_weak.upgrade();
    if (s.is_some()) {
      auto  arc      = std::move(s).unwrap();
      auto &state    = *arc;
      bool  expected = false;
      if (state.resolved.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        state.value = Option<ValueType>(std::move(value));
        state.waker.wake();
      }
    }
  }

  void resolve(const ValueType &value) {
    resolve(ValueType(value));
  }

  bool is_pending() const {
    auto s = m_weak.upgrade();
    if (s.is_none()) return false;
    auto arc = std::move(s).unwrap();
    return !arc->resolved.load(std::memory_order_acquire);
  }

private:
  explicit PromiseResolver(ArcWeak<_::ResolveState<ValueType>> weak) : m_weak(std::move(weak)) {}
  ArcWeak<_::ResolveState<ValueType>> m_weak;

  template <class U, class A> friend class _::AdapterPromiseNode;
  template <class U> friend std::pair<Promise<U>, PromiseResolver<U>> async();
};

/* ── PromiseResolver<void> ──────────────────────────────────────────── */

template <> class PromiseResolver<void> {
public:
  PromiseResolver()                                       = default;
  PromiseResolver(PromiseResolver &&) noexcept            = default;
  PromiseResolver &operator=(PromiseResolver &&) noexcept = default;
  PromiseResolver(const PromiseResolver &)                = delete;
  PromiseResolver &operator=(const PromiseResolver &)     = delete;

  void resolve() {
    auto s = m_weak.upgrade();
    if (s.is_some()) {
      auto  arc      = std::move(s).unwrap();
      auto &state    = *arc;
      bool  expected = false;
      if (state.resolved.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        state.waker.wake();
      }
    }
  }

  bool is_pending() const {
    auto s = m_weak.upgrade();
    if (s.is_none()) return false;
    auto arc = std::move(s).unwrap();
    return !arc->resolved.load(std::memory_order_acquire);
  }

private:
  explicit PromiseResolver(ArcWeak<_::ResolveState<Void>> weak) : m_weak(std::move(weak)) {}
  ArcWeak<_::ResolveState<Void>> m_weak;

  template <class U, class A> friend class _::AdapterPromiseNode;
  template <class U> friend std::pair<Promise<U>, PromiseResolver<U>> async();
};

} // namespace xpp

#endif // XPP_PROMISE_ADAPTER_H
