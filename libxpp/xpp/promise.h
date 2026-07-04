/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise.h - Promise<T> + PromiseResolver<T>.
 *
 * Promise<T> represents a value that will be available in the future.
 * Chain transformations with then(), wait for the result with wait().
 *
 * PromiseResolver<T>::create() returns a resolver; call .promise()
 * to get the associated Promise, and .resolve() to fulfill it.
 *
 * Works within a WaitScope — no separate runtime layer required.
 * The event loop drives poll/wake via xEventLoopPost.
 *
 * C++17-compatible.
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
template <class U> Own<PromiseNode<U>> _extract_node(Promise<U> &&);
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
 *     .wait();
 *   // result == 84
 * @endcode
 */
template <class T> class Promise {
public:
  using ValueType = typename FixVoid<T>::Type;

  Promise() : m_node(nullptr) {}
  explicit Promise(Own<_::PromiseNode<T>> node) : m_node(std::move(node)) {}
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
  auto
  then(Func &&func) -> Promise<typename _::ReducePromise<decltype(std::declval<Func>()())>::Type>;

  /// Discard the value, returning a completion-only Promise<void>.
  Promise<void> discard() {
    return then([](ValueType) {});
  }

  /// True if the promise holds a node (not empty).
  explicit operator bool() const {
    return m_node != nullptr;
  }

  /**
   * @brief Block until the promise resolves, driving the event loop.
   *
   * Must be called on the WaitScope thread. Polls the promise node;
   * if not ready, runs the event loop until the waker fires and
   * re-polls. Returns the resolved value. Consumes the promise.
   *
   * @par Thread safety
   * Not thread-safe. Only the WaitScope thread may call wait().
   * However, the promise being waited on may be resolved from
   * another thread via PromiseResolver::resolve() — that path is
   * thread-safe (AtomicPromiseWaker + atomic flag).
   *
   * @par Nested wait()
   * Safe to call wait() inside a callback that runs during another
   * wait(). xEventLoopRun no longer calls Enter/Leave internally,
   * so nested Run calls do not corrupt the thread-local loop binding.
   */
  ValueType wait() {
    XPP_ASSERT(m_node != nullptr, "wait() on empty promise");

    bool         done  = false;
    PromiseWaker waker = PromiseWaker::sync_wait(&done);

    while (true) {
      Option<ValueType> result = m_node->poll(waker);
      if (result.is_some()) {
        return result.unwrap();
      }
      // Pending — run one loop iteration per try. X_RUN_ONCE returns
      // after processing one batch of events (timers, I/O, done queue),
      // so we can re-check `done` without waiting for all event sources
      // to drain. This is critical for race(): with two timers, the
      // faster timer sets done=true, but the slower timer keeps the
      // loop alive. X_RUN_ONCE returns after the faster timer fires,
      // allowing re-poll before the slower timer completes.
      while (!done) {
        xEventLoopRun(EventLoop::current(), X_RUN_ONCE);
      }
      done = false; // reset for next poll iteration
    }
  }

private:
  Own<_::PromiseNode<T>> m_node;

  template <class U> friend class Promise;
  template <class U> friend class _::ChainPromiseNode;

  /// Internal: extract node from a moved Promise. Used by combinators.
  template <class U> friend Own<_::PromiseNode<U>> _::_extract_node(Promise<U> &&);
};

/* ── Free helper functions ──────────────────────────────────────── */

inline Promise<void> yield() {
  return Promise<void>(Own<_::PromiseNode<void>>(new _::YieldPromiseNode()));
}

/* ── Chain helper ────────────────────────────────────────────────── */

namespace _ {

namespace _chain {

template <class ReducedT, class OutT, class T, class Func>
typename std::enable_if<std::is_same<OutT, ReducedT>::value, Own<PromiseNode<ReducedT>>>::type
chain(Own<PromiseNode<T>> dep, Func &&func) {
  return Own<PromiseNode<ReducedT>>(
    new TransformPromiseNode<ReducedT, T, Func>(std::move(dep), std::forward<Func>(func)));
}

template <class ReducedT, class OutT, class T, class Func>
typename std::enable_if<!std::is_same<OutT, ReducedT>::value, Own<PromiseNode<ReducedT>>>::type
chain(Own<PromiseNode<T>> dep, Func &&func) {
  return Own<PromiseNode<ReducedT>>(new ChainPromiseNode<ReducedT>(
    Own<PromiseNode<Promise<ReducedT>>>(new TransformPromiseNode<Promise<ReducedT>, T, Func>(
      std::move(dep), std::forward<Func>(func)))));
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

  Own<_::PromiseNode<T>> dep(std::move(m_node));
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

  Own<_::PromiseNode<void>> dep(std::move(m_node));
  auto                      node =
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
  auto *node = new _::AdapterPromiseNode<T, Adapter>(std::forward<AdapterArgs>(args)...);
  return Promise<T>(Own<_::PromiseNode<T>>(node));
}

/** Create an immediately-resolved promise. T deduced from argument. */
template <class T> Promise<T> resolve(T v) {
  return Promise<T>(Own<_::PromiseNode<T>>(new _::ImmediatePromiseNode<T>(std::move(v))));
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
  auto promise  = Promise<T>(Own<_::PromiseNode<T>>(new _::ManualResolveNode<T>(std::move(state))));
  return {std::move(promise), std::move(resolver)};
}

} // namespace xpp

#if XPP_HAS_COROUTINES
#include <xpp/promise_coroutine.h>
#endif

#endif // XPP_PROMISE_H