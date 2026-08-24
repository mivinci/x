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
#include <xpp/void.h>

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

template <class T> class PromiseResolver;

namespace _ {

/* ── ResolveState<T> ─────────────────────────────────────────────── */

/**
 * @brief Shared state behind every xpp::async() / adapt() promise.
 *
 * Tri-state publication protocol: resolve() claims Waiting→Resolving
 * (exactly-once via CAS), writes the payload, then release-stores
 * Ready. A poller only reads the payload after an acquire-load of
 * Ready — that load/release-store pair is the happens-before edge that
 * makes the payload safe to read. (A two-state bool resolved *before*
 * the payload write left the write unordered with concurrent pollers:
 * a poller could acquire the flag and race on `value` — TSan-visible,
 * a torn read for non-word-sized types.)
 */
template <class T> struct ResolveState {
  enum State : uint8_t {
    Waiting   = 0, ///< No resolve() in progress.
    Resolving = 1, ///< resolve() claimed the state; payload write in flight.
    Ready     = 2, ///< Payload published (release); safe to read.
  };

  Option<T>          value;
  AtomicPromiseWaker waker;
  std::atomic<State> state{Waiting};
};

/* ── poll_state helper (shared by all node types) ────────────────── */

template <class T> inline Option<T> poll_state(ResolveState<T> &s, const PromiseContext &cx) {
  if (s.state.load(std::memory_order_acquire) == ResolveState<T>::Ready) {
    return std::move(s.value);
  }
  s.waker.register_by_ref(cx.waker());
  if (s.state.load(std::memory_order_acquire) == ResolveState<T>::Ready) {
    s.waker.wake();
    return std::move(s.value);
  }
  return none;
}

inline Option<Void> poll_state(ResolveState<Void> &s, const PromiseContext &cx) {
  if (s.state.load(std::memory_order_acquire) == ResolveState<Void>::Ready) {
    return Option<Void>(Void{});
  }
  s.waker.register_by_ref(cx.waker());
  if (s.state.load(std::memory_order_acquire) == ResolveState<Void>::Ready) {
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
  Option<ValueType> poll(const PromiseContext &cx) override {
    return poll_state(*m_state, cx);
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

  Option<ValueType> poll(const PromiseContext &cx) override {
    return poll_state(*m_state, cx);
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
      auto  expected = _::ResolveState<ValueType>::Waiting;
      /* Claim first (exactly-once)... */
      if (state.state.compare_exchange_strong(expected, _::ResolveState<ValueType>::Resolving,
                                              std::memory_order_acq_rel)) {
        state.value = Option<ValueType>(std::move(value));
        /* ...publish last: this release-store is the happens-before edge
         * that makes the payload visible to poll_state's acquire-load
         * of Ready. */
        state.state.store(_::ResolveState<ValueType>::Ready, std::memory_order_release);
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
    return arc->state.load(std::memory_order_acquire) != _::ResolveState<ValueType>::Ready;
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
      auto  expected = _::ResolveState<Void>::Waiting;
      if (state.state.compare_exchange_strong(expected, _::ResolveState<Void>::Resolving,
                                              std::memory_order_acq_rel)) {
        state.state.store(_::ResolveState<Void>::Ready, std::memory_order_release);
        state.waker.wake();
      }
    }
  }

  bool is_pending() const {
    auto s = m_weak.upgrade();
    if (s.is_none()) return false;
    auto arc = std::move(s).unwrap();
    return arc->state.load(std::memory_order_acquire) != _::ResolveState<Void>::Ready;
  }

private:
  explicit PromiseResolver(ArcWeak<_::ResolveState<Void>> weak) : m_weak(std::move(weak)) {}
  ArcWeak<_::ResolveState<Void>> m_weak;

  template <class U, class A> friend class _::AdapterPromiseNode;
  template <class U> friend std::pair<Promise<U>, PromiseResolver<U>> async();
};

} // namespace xpp

#endif // XPP_PROMISE_ADAPTER_H
