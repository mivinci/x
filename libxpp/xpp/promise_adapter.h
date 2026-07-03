/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter.h - Adapter pattern + PromiseResolver for Promise.
 *
 * PromiseResolver<T> holds an ArcWeak to shared ResolveState — safe to
 * call after the Promise is destroyed (resolve() silently drops).
 *
 * AdaptedPromiseNode<T, Adapter> owns an Adapter, shares state via Arc.
 * TimerAdapter replaces TimerPromiseNode (~50 → ~15 lines).
 *
 * C++17-compatible. Header-only.
 */
#ifndef XPP_PROMISE_ADAPTER_H
#define XPP_PROMISE_ADAPTER_H

#include <atomic>
#include <utility>

#include <xpp/arc.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/promise.h>
#include <xpp/promise_node.h>
#include <xpp/promise_waker.h>
#include <xpp/void.h>

#include <x/base/event.h>

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

template <class T> class PromiseResolver;

namespace _ {

/* ── ResolveState<T> ─────────────────────────────────────────────── */
/*
 * Shared state between the node (strong ref via Arc) and PromiseResolver
 * (weak ref via ArcWeak). Outlives the node — when the node is
 * destroyed, strong count → 0; PromiseResolver::upgrade() returns None.
 */
template <class T> struct ResolveState {
  Option<T>          value;
  PromiseAtomicWaker waker;
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

// Void specialization: resolve() doesn't set value, so return Some(Void{})
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
/*
 * Node for newPromiseAndResolver — no Adapter, just polls shared state.
 */
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

/* ── AdaptedPromiseNode<T, Adapter> ──────────────────────────────── */
/*
 * Generic adapter node. Owns an Adapter and an Arc<ResolveState<T>>.
 * The Adapter does async work and calls PromiseResolver::resolve() when
 * done. poll() is generic (checks resolved + registers waker).
 *
 * Member init order: m_state (Arc) → m_adapter (receives PromiseResolver
 * from m_state's ArcWeak). Destruction: ~Adapter (cancel) → ~Arc.
 */
template <class T, class Adapter> class AdaptedPromiseNode : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  template <class... AdapterArgs>
  explicit AdaptedPromiseNode(AdapterArgs &&...args)
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

/* ── Forward declarations ── */
// async<T>() defined below

/* ── PromiseResolver<T> ─────────────────────────────────────────────── */
/*
 * Safe resolver handle. Holds ArcWeak<ResolveState<T>> — if the
 * Promise is destroyed, upgrade() returns None and resolve() drops.
 * Thread-safe (ArcWeak::upgrade is a CAS loop).
 */
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

  template <class U, class A> friend class _::AdaptedPromiseNode;
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

  template <class U, class A> friend class _::AdaptedPromiseNode;
  template <class U> friend std::pair<Promise<U>, PromiseResolver<U>> async();
};

/* ── TimerAdapter ────────────────────────────────────────────────── */
/*
 * Replaces TimerPromiseNode. Owns an xTimer; callback calls
 * m_resolver.resolve(). Destructor calls xTimerStop (synchronous on
 * the event loop thread — callback can't fire after stop).
 */
class TimerAdapter {
public:
  TimerAdapter(PromiseResolver<void> &&resolver, uint64_t ms) : m_resolver(std::move(resolver)) {
    m_handle = xTimerStart(
      [](void *a) {
        auto *self = static_cast<TimerAdapter *>(a);
        self->m_fired.store(true, std::memory_order_release);
        self->m_resolver.resolve();
      },
      this,
      [](void *a) {
        auto *self     = static_cast<TimerAdapter *>(a);
        self->m_handle = nullptr;
        self->m_fired.store(true, std::memory_order_release);
      },
      ms, 0);
  }

  ~TimerAdapter() {
    if (!m_fired.load(std::memory_order_acquire) && m_handle) {
      xTimerStop(m_handle);
    }
  }

  TimerAdapter(const TimerAdapter &)            = delete;
  TimerAdapter &operator=(const TimerAdapter &) = delete;
  TimerAdapter(TimerAdapter &&)                 = delete;
  TimerAdapter &operator=(TimerAdapter &&)      = delete;

private:
  xTimer                m_handle;
  std::atomic<bool>     m_fired{false};
  PromiseResolver<void> m_resolver;
};

/* ── Factory: newAdaptedPromise ──────────────────────────────────── */

template <class T, class Adapter, class... AdapterArgs>
Promise<T> newAdaptedPromise(AdapterArgs &&...args) {
  auto *node = new _::AdaptedPromiseNode<T, Adapter>(std::forward<AdapterArgs>(args)...);
  return Promise<T>(Own<_::PromiseNode<T>>(node));
}

/* ── Factory: async ──────────────────────────────────────────────── */

template <class T> std::pair<Promise<T>, PromiseResolver<T>> async() {
  using V       = typename FixVoid<T>::Type;
  auto state    = Arc<_::ResolveState<V>>::make();
  auto resolver = PromiseResolver<T>(Arc<_::ResolveState<V>>::downgrade(state));
  auto promise  = Promise<T>(Own<_::PromiseNode<T>>(new _::ManualResolveNode<T>(std::move(state))));
  return {std::move(promise), std::move(resolver)};
}

} // namespace xpp

#endif // XPP_PROMISE_ADAPTER_H
