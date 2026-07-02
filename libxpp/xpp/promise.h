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
#include <xpp/promise_node.h>
#include <xpp/void.h>

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

template <class T> class PromiseResolver;

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
 *   any thread — the AdapterPromiseNode uses PromiseAtomicWaker +
 *   atomic flag to coordinate concurrent poll and resolve without
 *   a mutex.
 *
 * @code
 *   xpp::EventLoop loop;
 *   xpp::WaitScope scope(loop);
 *
 *   int result = xpp::Promise<int>::resolve(42)
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
    -> Promise<typename ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type>;

  template <class Func, class V = ValueType,
            class = typename std::enable_if<std::is_same<V, Void>::value>::type, class = void>
  auto then(Func &&func) -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type>;

  /// Discard the value, returning a completion-only Promise<void>.
  Promise<void> discard() {
    return then([](ValueType) {});
  }

  /// Create an immediately-resolved promise.
  static Promise resolve(ValueType value) {
    Own<_::PromiseNode<T>> node(new _::ImmediatePromiseNode<T>(std::move(value)));
    return Promise(std::move(node));
  }

  template <class V = ValueType,
            class   = typename std::enable_if<std::is_same<V, Void>::value>::type>
  static Promise resolve() {
    Own<_::PromiseNode<T>> node(new _::ImmediatePromiseNode<void>());
    return Promise(std::move(node));
  }

  /// Evaluate a synchronous function as a promise.
  template <class Func, class V = ValueType,
            class = typename std::enable_if<std::is_same<V, Void>::value>::type>
  static auto eval(Func &&func) -> Promise<typename ReducePromise<ReturnTypeVoid<Func>>::Type> {
    return Promise<void>(Own<_::PromiseNode<void>>(new _::YieldPromiseNode()))
      .then(std::forward<Func>(func));
  }

  /**
   * @brief Return a Promise that resolves after `ms` milliseconds.
   *
   * Schedules a one-shot timer on the current event loop via
   * xTimerStart. When the timer fires, the promise resolves.
   *
   * Must be called from within a WaitScope.
   *
   * @param ms  Delay in milliseconds. 0 = "next iteration".
   * @return    Promise<void> that resolves after the delay.
   *
   * @code
   *   Promise<void>::after(100).then([]() { ... }).wait();
   * @endcode
   */
  template <class V = ValueType,
            class = typename std::enable_if<std::is_same<V, Void>::value>::type, class = void>
  static Promise<void> after(uint64_t ms);

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
   * thread-safe (PromiseAtomicWaker + atomic flag).
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
      // Pending — run the loop until waker fires
      while (!done) {
        xEventLoopRun(EventLoop::current(), X_RUN_DEFAULT);
      }
      done = false; // reset for next poll iteration
    }
  }

private:
  Own<_::PromiseNode<T>> m_node;

  template <class U> friend class Promise;
  template <class U> friend class PromiseResolver;
  template <class U> friend class _::ChainPromiseNode;
};

/* ── PromiseResolver<T> ─────────────────────────────────────────── */

/**
 * @brief Manual resolver for a deferred Promise.
 *
 * Create via PromiseResolver<T>::create(), then call .promise() to
 * obtain the associated Promise and .resolve() to fulfill it.
 *
 * Move-only. Holds a raw pointer to the AdapterPromiseNode (ownership
 * stays with the Promise's Own<PromiseNode<T>>).
 *
 * @par Thread safety
 * resolve() is thread-safe — may be called from any thread. The
 * AdapterPromiseNode uses PromiseAtomicWaker (lock-free 2-bit state
 * machine) and std::atomic<bool> for the resolved flag, so concurrent
 * poll (loop thread) and resolve (any thread) are safe without a
 * mutex.
 *
 * is_pending() is not atomic — only call on the thread that owns
 * the PromiseResolver.
 *
 * @code
 *   auto r = xpp::PromiseResolver<int>::create();
 *   auto p = r.promise();
 *   // ... pass r to an async operation ...
 *   // ... later, from any thread:
 *   r.resolve(42);
 *   // ... in WaitScope:
 *   EXPECT_EQ(p.wait(), 42);
 * @endcode
 */
template <class T> class PromiseResolver {
public:
  using ValueType = typename FixVoid<T>::Type;

  PromiseResolver(PromiseResolver &&o) noexcept : m_node(o.m_node) {
    o.m_node = nullptr;
  }
  PromiseResolver &operator=(PromiseResolver &&o) noexcept {
    m_node   = o.m_node;
    o.m_node = nullptr;
    return *this;
  }
  PromiseResolver(const PromiseResolver &)            = delete;
  PromiseResolver &operator=(const PromiseResolver &) = delete;

  /**
   * @brief Obtain the associated Promise.
   *
   * Can be called at most once. The Promise takes ownership of the
   * AdapterPromiseNode; after this call, the resolver still holds
   * a raw pointer to the node (for resolve()), but lifecycle is
   * managed by the Promise.
   */
  Promise<T> promise() {
    XPP_ASSERT(m_node != nullptr, "promise() on empty or already-consumed resolver");
    auto *node = m_node;
    // Don't null m_node — resolve() still needs it.
    // Lifecycle is transferred to Promise's Own<>.
    return Promise<T>(Own<_::PromiseNode<T>>(static_cast<_::PromiseNode<T> *>(node)));
  }

  void resolve(ValueType &&value) {
    XPP_ASSERT(m_node != nullptr, "PromiseResolver: already consumed or moved-from");
    m_node->resolve(std::move(value));
    m_node = nullptr;
  }

  void resolve(const ValueType &value) {
    resolve(ValueType(value));
  }

  bool is_pending() const {
    return m_node != nullptr;
  }

  /**
   * @brief Create a PromiseResolver for deferred resolution.
   *
   * The returned resolver holds a raw pointer to a newly-allocated
   * AdapterPromiseNode. Call .promise() to obtain the Promise (which
   * takes ownership of the node), then call .resolve() to fulfill it.
   */
  static PromiseResolver create() {
    auto *adapter = new _::AdapterPromiseNode<ValueType>();
    return PromiseResolver(adapter);
  }

private:
  explicit PromiseResolver(_::AdapterPromiseNode<ValueType> *node) : m_node(node) {}

  _::AdapterPromiseNode<ValueType> *m_node;
};

template <> class PromiseResolver<void> {
public:
  PromiseResolver(PromiseResolver &&o) noexcept : m_node(o.m_node) {
    o.m_node = nullptr;
  }
  PromiseResolver &operator=(PromiseResolver &&o) noexcept {
    m_node   = o.m_node;
    o.m_node = nullptr;
    return *this;
  }
  PromiseResolver(const PromiseResolver &)            = delete;
  PromiseResolver &operator=(const PromiseResolver &) = delete;

  Promise<void> promise() {
    XPP_ASSERT(m_node != nullptr, "promise() on empty or already-consumed resolver");
    return Promise<void>(Own<_::PromiseNode<void>>(static_cast<_::PromiseNode<void> *>(m_node)));
  }

  void resolve() {
    XPP_ASSERT(m_node != nullptr, "PromiseResolver: already consumed or moved-from");
    m_node->resolve();
    m_node = nullptr;
  }

  bool is_pending() const {
    return m_node != nullptr;
  }

  static PromiseResolver create() {
    auto *adapter = new _::AdapterPromiseNode<Void>();
    return PromiseResolver(adapter);
  }

private:
  explicit PromiseResolver(_::AdapterPromiseNode<Void> *node) : m_node(node) {}

  _::AdapterPromiseNode<Void> *m_node;
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
  -> Promise<typename ReducePromise<decltype(std::declval<Func>()(std::declval<V>()))>::Type> {
  using RawU     = decltype(std::declval<Func>()(std::declval<V>()));
  using ReducedT = typename ReducePromise<RawU>::Type;
  using OutT     = RawU;

  Own<_::PromiseNode<T>> dep(std::move(m_node));
  auto node = _::_chain::chain<ReducedT, OutT, T, Func>(std::move(dep), std::forward<Func>(func));
  return Promise<ReducedT>(std::move(node));
}

template <class T>
template <class Func, class V, class, class>
auto Promise<T>::then(Func &&func)
  -> Promise<typename ReducePromise<decltype(std::declval<Func>()())>::Type> {
  using RawU     = decltype(std::declval<Func>()());
  using ReducedT = typename ReducePromise<RawU>::Type;
  using OutT     = RawU;

  Own<_::PromiseNode<void>> dep(std::move(m_node));
  auto                      node =
    _::_chain::chain<ReducedT, OutT, void, Func>(std::move(dep), std::forward<Func>(func));
  return Promise<ReducedT>(std::move(node));
}

/* ── Promise<T>::after ──────────────────────────────────────── */

template <class T>
template <class V, class, class>
inline Promise<void> Promise<T>::after(uint64_t ms) {
  auto *resolver = new PromiseResolver<void>(PromiseResolver<void>::create());
  auto  promise  = resolver->promise();
  xTimerStart(
    [](void *arg) {
      auto *r = static_cast<PromiseResolver<void> *>(arg);
      r->resolve();
      delete r;
    },
    resolver, NULL, ms, 0);
  return promise;
}

} // namespace xpp

#endif // XPP_PROMISE_H
