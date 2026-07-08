/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise.h - Promise<T> + PromiseResolver<T>.
 *
 * Promise<T> represents a value that will be available in the future.
 * Chain transformations with then(), block for the result with await().
 *
 * PromiseResolver<T>::create() returns a resolver; call .promise()
 * to get the associated Promise, and .resolve() to fulfill it.
 *
 * Works within a WaitScope — no separate runtime layer required.
 * The event loop drives poll/wake via xEventLoopPost.
 *
 * C++11-compatible.
 */

#ifndef XPP_PROMISE_H
#define XPP_PROMISE_H

#include <utility>

#include <xpp/compiler.h>
#include <xpp/event.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/promise_adapter.h>
#include <xpp/promise_adapter_timer.h>
#include <xpp/promise_adapter_work.h>
#include <xpp/promise_node.h>
#include <xpp/void.h>

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

namespace _ {
/// Forward declaration so Promise<T> can befriend it.
template <class U> _::OwnPromiseNode<U> _extract_node(Promise<U> &&);
template <class T, class Adapter> class AdapterPromiseNode;
template <class T, class Func> class WorkAdapter;
} // namespace _

/* ── ReturnType helper ──────────────────────────────────────────── */

template <class Func, class T> using ReturnType = decltype(std::declval<Func>()(std::declval<T>()));

template <class Func> using ReturnTypeVoid = decltype(std::declval<Func>()());

/* ── Promise<T> ──────────────────────────────────────────────────── */

/**
 * @brief A composable deferred value.
 * @tparam T  The value type. Use void for completion-only promises.
 *
 * Move-only. Owns a PromiseNode<T> internally.
 *
 * @par Thread safety
 * - @b wait(): Must be called on the WaitScope thread. It drives
 *   xEventLoopRun internally.
 * - @b then() / resolve(): Not thread-safe; call before sharing
 *   across threads.
 * - @b PromiseResolver::resolve(): Thread-safe. May be called from
 *   any thread — the AdapterPromiseNode uses AtomicPromiseWaker +
 *   atomic flag to coordinate concurrent poll and resolve without
 *   a mutex.
 *
 * @code
 *   xpp::EventLoop loop;
 *   xpp::WaitScope scope(loop);
 *
 *   int result = xpp::resolve(42)
 *     .then([](int x) { return x * 2; })
 *     .await();
 *   // result == 84
 * @endcode
 */
template <class T> class Promise {
public:
  using ValueType = typename FixVoid<T>::Type;

  Promise() : m_node(nullptr) {}
  explicit Promise(_::OwnPromiseNode<T> node) : m_node(std::move(node)) {}
  Promise(Promise &&o) noexcept : m_node(std::move(o.m_node)) {}
  Promise &operator=(Promise &&o) noexcept {
    m_node = std::move(o.m_node);
    return *this;
  }
  Promise(const Promise &)            = delete;
  Promise &operator=(const Promise &) = delete;

  /**
   * @brief Chain a transformation on the resolved value.
   *
   * If func returns U, returns Promise<U>.
   * If func returns Promise<U>, returns Promise<U> (auto-flatten).
   */
  template <class Func, class V = ValueType,
            class = typename std::enable_if<!std::is_same<V, Void>::value>::type>
  auto then(Func &&func)
    -> Promise<typename _::ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type>;

  template <class Func, class V = ValueType,
            class = typename std::enable_if<std::is_same<V, Void>::value>::type, class = void>
  auto then(Func &&func)
    -> Promise<typename _::ReducePromise<decltype(std::declval<Func>()())>::Type>;

  /// Discard the value, returning a completion-only Promise<void>.
  Promise<void> discard() {
    return then([](ValueType) {});
  }

  /// True if the promise holds a node (not empty).
  explicit operator bool() const {
    return m_node != nullptr;
  }

  /**
   * @brief Wait for the promise to resolve.
   *
   * Must be called on the WaitScope thread. Polls the promise node;
   * if not ready, parks the current context until the waker fires and
   * re-polls. Returns the resolved value. Consumes the promise.
   *
   * @par Parking
   * PromiseWaker::park() encapsulates the waiting strategy:
   *   - Non-fiber: runs xEventLoopRun(X_RUN_ONCE) in a poll loop.
   *   - Fiber:     suspends via xFiberYield(), yielding to the event
   *                loop until the waker switches the fiber back in.
   *
   * @par Thread safety
   * Not thread-safe. Only the WaitScope thread may call await().
   * However, the promise being waited on may be resolved from
   * another thread via PromiseResolver::resolve() — that path is
   * thread-safe (AtomicPromiseWaker + atomic flag).
   *
   * @par Nested await()
   * Safe to call await() inside a callback that runs during another
   * await(). xEventLoopRun no longer calls Enter/Leave internally,
   * so nested Run calls do not corrupt the thread-local loop binding.
   */
  ValueType await() {
    XPP_ASSERT(m_node != nullptr, "await() on empty promise");

    PromiseWaker waker;
    while (true) {
      Option<ValueType> result = m_node->poll(waker);
      if (result.is_some()) {
        return std::move(result).unwrap();
      }
      waker.park();
    }
  }

  /// @deprecated Use await() instead.
  XPP_DEPRECATED("use await() instead")
  ValueType wait() { return await(); }

private:
  _::OwnPromiseNode<T> m_node;

  template <class U> friend class Promise;
  template <class U> friend class _::ChainPromiseNode;

  /// Internal: extract node from a moved Promise. Used by combinators.
  template <class U> friend _::OwnPromiseNode<U> _::_extract_node(Promise<U> &&);
};

/* ── Free helper functions ──────────────────────────────────────── */

inline Promise<void> yield() {
  return Promise<void>(_::OwnPromiseNode<void>(_::promise::allocate<_::YieldPromiseNode>(nullptr)));
}

/* ── Chain helper ────────────────────────────────────────────────── */

namespace _ {

namespace _chain {

// chain: append a TransformPromiseNode to the predecessor's arena.
// Uses _::promise::append which reads the arena from the predecessor's
// header and transfers ownership to the new node.
template <class ReducedT, class OutT, class T, class Func>
typename std::enable_if<std::is_same<OutT, ReducedT>::value, _::OwnPromiseNode<ReducedT>>::type
chain(_::OwnPromiseNode<T> dep, Func &&func) {
  void *pred = dep.get();
  return _::OwnPromiseNode<ReducedT>(_::promise::append<TransformPromiseNode<ReducedT, T, Func>>(
    pred, std::move(dep), std::forward<Func>(func)));
}

template <class ReducedT, class OutT, class T, class Func>
typename std::enable_if<!std::is_same<OutT, ReducedT>::value, _::OwnPromiseNode<ReducedT>>::type
chain(_::OwnPromiseNode<T> dep, Func &&func) {
  void *pred = dep.get();
  // Inner: TransformPromiseNode<Promise<ReducedT>, T, Func> (appended to dep's arena)
  auto inner = _::OwnPromiseNode<Promise<ReducedT>>(
    _::promise::append<TransformPromiseNode<Promise<ReducedT>, T, Func>>(pred, std::move(dep),
                                                                         std::forward<Func>(func)));
  // Outer: ChainPromiseNode<ReducedT> (appended to inner's arena)
  void *inner_pred = inner.get();
  return _::OwnPromiseNode<ReducedT>(
    _::promise::append<ChainPromiseNode<ReducedT>>(inner_pred, std::move(inner)));
}

} // namespace _chain

} // namespace _

/* ── Promise<T>::then implementations ──────────────────────────── */

template <class T>
template <class Func, class V, class>
auto Promise<T>::then(Func &&func)
  -> Promise<typename _::ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type> {
  using RawU     = decltype(std::declval<Func>()(std::declval<V>()));
  using ReducedT = typename _::ReducePromise<RawU>::Type;
  using OutT     = RawU;

  _::OwnPromiseNode<T> dep(std::move(m_node));
  auto node = _::_chain::chain<ReducedT, OutT, T, Func>(std::move(dep), std::forward<Func>(func));
  return Promise<ReducedT>(std::move(node));
}

template <class T>
template <class Func, class V, class, class>
auto Promise<T>::then(Func &&func)
  -> Promise<typename _::ReducePromise<decltype(std::declval<Func>()())>::Type> {
  using RawU     = decltype(std::declval<Func>()());
  using ReducedT = typename _::ReducePromise<RawU>::Type;
  using OutT     = RawU;

  _::OwnPromiseNode<void> dep(std::move(m_node));
  auto                    node =
    _::_chain::chain<ReducedT, OutT, void, Func>(std::move(dep), std::forward<Func>(func));
  return Promise<ReducedT>(std::move(node));
}

/* ── Forward declarations ───────────────────────────────────────── */

class TimerAdapter;
template <class T> class PromiseResolver;
template <class T> std::pair<Promise<T>, PromiseResolver<T>> async();

/* ── Free function factories ────────────────────────────────────── */

/** Create a promise backed by a custom Adapter. */
template <class T, class Adapter, class... AdapterArgs> Promise<T> adapt(AdapterArgs &&...args) {
  auto *node = _::promise::allocate<_::AdapterPromiseNode<T, Adapter>>(
    nullptr, std::forward<AdapterArgs>(args)...);
  return Promise<T>(_::OwnPromiseNode<T>(node));
}

/** Create an immediately-resolved promise. T deduced from argument. */
template <class T> Promise<T> resolve(T v) {
  return Promise<T>(
    _::OwnPromiseNode<T>(_::promise::allocate<_::ImmediatePromiseNode<T>>(nullptr, std::move(v))));
}

/**
 * @brief Create an immediately-resolved Promise<void>.
 *
 * The void counterpart of resolve(v) — no value to move, no template
 * deduction needed. Returns a promise that is already fulfilled in a
 * single arena allocation (8B bump inside the head node's arena).
 *
 * For chaining after flush(), close(), or any async operation whose
 * only job is to signal completion.
 *
 * @code
 *   Promise<void> flush() override {
 *     if (m_pos == 0) return xpp::resolve();  // nothing to flush
 *     return m_writer.write(m_buf, m_pos).then([...](ssize_t) {
 *       return xpp::resolve();
 *     });
 *   }
 * @endcode
 */
inline Promise<void> resolve() {
  return Promise<void>(
    _::OwnPromiseNode<void>(_::promise::allocate<_::ImmediatePromiseNode<void>>(nullptr)));
}

/** Resolve after `ms` milliseconds. Always returns Promise<void>. */
inline Promise<void> after(uint64_t ms) {
  return adapt<void, TimerAdapter>(ms);
}

/** Defer a synchronous function as a promise (runs on first poll). */
template <class Func>
auto defer(Func &&fn) -> Promise<typename _::ReducePromise<_::ReturnTypeVoid<Func>>::Type> {
  return yield().then(std::forward<Func>(fn));
}

/** Submit work to the thread pool, return a Promise for the result. */
template <class Func>
auto work(Func &&fn) -> Promise<typename std::decay<decltype(std::declval<Func>()())>::type> {
  using T = typename std::decay<decltype(std::declval<Func>()())>::type;
  return adapt<T, _::WorkAdapter<T, typename std::decay<Func>::type>>(std::forward<Func>(fn));
}

/* ── async() ────────────────────────────────────────────────────── */

template <class T> std::pair<Promise<T>, PromiseResolver<T>> async() {
  using V       = typename FixVoid<T>::Type;
  auto state    = Arc<_::ResolveState<V>>::make();
  auto resolver = PromiseResolver<T>(Arc<_::ResolveState<V>>::downgrade(state));
  auto promise  = Promise<T>(
    _::OwnPromiseNode<T>(_::promise::allocate<_::ManualResolveNode<T>>(nullptr, std::move(state))));
  return {std::move(promise), std::move(resolver)};
}

} // namespace xpp

#if XPP_HAS_COROUTINES
#include <xpp/promise_coroutine.h>
#endif

#endif // XPP_PROMISE_H