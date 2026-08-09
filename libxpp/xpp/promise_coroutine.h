/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_coroutine.h — C++20 coroutine support for Promise<T>.
 *
 * Promise<T> is both a coroutine return type (via std::coroutine_traits)
 * and an awaitable (via ADL operator co_await). No Task<T> wrapper.
 *
 * Requires C++20 coroutines. Guarded by XPP_HAS_COROUTINES.
 */
#ifndef XPP_PROMISE_COROUTINE_H
#define XPP_PROMISE_COROUTINE_H

#if !XPP_HAS_COROUTINES
#error "promise_coroutine.h requires C++20 coroutine support"
#endif

#include <coroutine>
#include <exception>
#include <utility>

#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/promise_adapter.h>
#include <xpp/promise_combinators.h>
#include <xpp/promise_node.h>
#include <xpp/promise_context.h>
#include <xpp/void.h>

namespace xpp {

/* ── Forward declarations ────────────────────────────────────────── */

template <class T> class PromiseAwaiter;

namespace _ {

/* ── AwaitState (type-erased) ────────────────────────────────────── */

struct AwaitState {
  virtual ~AwaitState()                        = default;
  virtual bool poll(const PromiseContext &cx) = 0;
};

template <class U> struct AwaitStateImpl : AwaitState {
  _::OwnPromiseNode<U> m_node;
  Option<U>           *m_value_ptr;

  AwaitStateImpl(_::OwnPromiseNode<U> n, Option<U> *vp) : m_node(std::move(n)), m_value_ptr(vp) {}

  bool poll(const PromiseContext &cx) override {
    auto r = m_node->poll(cx);
    if (r.is_none()) return false;
    *m_value_ptr = std::move(r);
    return true;
  }
};

/* Void specialization: PromiseNode<void> returns Option<Void>,
 * but we just need a "ready" flag, not a value. */
struct VoidAwaitState : AwaitState {
  _::OwnPromiseNode<void> m_node;
  bool                   *m_ready_ptr;

  VoidAwaitState(_::OwnPromiseNode<void> n, bool *rp) : m_node(std::move(n)), m_ready_ptr(rp) {}

  bool poll(const PromiseContext &cx) override {
    auto r = m_node->poll(cx);
    if (r.is_none()) return false;
    *m_ready_ptr = true;
    return true;
  }
};

/* ── CoroutinePromiseNode<T> ─────────────────────────────────────── */

template <class T> class CoroutinePromiseNode : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  std::coroutine_handle<> m_handle;
  Option<ValueType>       m_result;
  bool                    m_done = false;
  std::exception_ptr      m_exception;
  Own<AwaitState>         m_await_state;

  Option<ValueType> poll(const PromiseContext &cx) override {
    while (true) {
      if (m_done) {
        if (m_exception) std::rethrow_exception(m_exception);
        return std::move(m_result);
      }

      if (m_await_state) {
        /* Poll the awaited promise. If not ready, return None. */
        if (!m_await_state->poll(cx)) return none;
        /* Ready — clear await state and resume coroutine. */
        m_await_state.reset();
        m_handle.resume();
        /* Loop: coroutine may have co_returned or co_awaited again. */
      } else {
        /* No await state — start or resume the coroutine. */
        m_handle.resume();
        /* Loop: check m_done or new m_await_state. */
      }
    }
  }

  template <class U> void set_await(_::OwnPromiseNode<U> node, Option<U> *value_ptr) {
    m_await_state = Own<AwaitState>(new AwaitStateImpl<U>(std::move(node), value_ptr));
  }

  void set_await_void(_::OwnPromiseNode<void> node, bool *ready_ptr) {
    m_await_state = Own<AwaitState>(new VoidAwaitState(std::move(node), ready_ptr));
  }

  ~CoroutinePromiseNode() {
    if (m_handle && !m_done) m_handle.destroy();
  }
};

/* ── CoroutinePromise<T> (promise_type for Promise<T>, T != void) ── */

template <class T> class CoroutinePromise {
public:
  CoroutinePromiseNode<T> *m_node = nullptr;

  Promise<T> get_return_object() {
    auto *n     = _::promise::allocate<CoroutinePromiseNode<T>>(nullptr);
    m_node      = n;
    n->m_handle = std::coroutine_handle<CoroutinePromise>::from_promise(*this);
    return Promise<T>(_::OwnPromiseNode<T>(n));
  }

  std::suspend_always initial_suspend() noexcept {
    return {};
  }
  std::suspend_always final_suspend() noexcept {
    return {};
  }

  void return_value(T v) {
    m_node->m_result = Option<T>(std::move(v));
    m_node->m_done   = true;
  }

  void unhandled_exception() {
    m_node->m_exception = std::current_exception();
    m_node->m_done      = true;
  }

  static Promise<T> get_return_object_on_allocation_failure() {
    return Promise<T>();
  }
};

/* ── CoroutinePromise<void> (promise_type for Promise<void>) ─────── */

template <> class CoroutinePromise<void> {
public:
  CoroutinePromiseNode<void> *m_node = nullptr;

  Promise<void> get_return_object() {
    auto *n     = _::promise::allocate<CoroutinePromiseNode<void>>(nullptr);
    m_node      = n;
    n->m_handle = std::coroutine_handle<CoroutinePromise<void>>::from_promise(*this);
    return Promise<void>(_::OwnPromiseNode<void>(n));
  }

  std::suspend_always initial_suspend() noexcept {
    return {};
  }
  std::suspend_always final_suspend() noexcept {
    return {};
  }

  void return_void() {
    m_node->m_result = Option<Void>(Void{});
    m_node->m_done   = true;
  }

  void unhandled_exception() {
    m_node->m_exception = std::current_exception();
    m_node->m_done      = true;
  }

  static Promise<void> get_return_object_on_allocation_failure() {
    return Promise<void>();
  }
};

} // namespace _

/* ── PromiseAwaiter<T> (T != void) ───────────────────────────────── */

template <class T> class PromiseAwaiter {
public:
  PromiseAwaiter(Promise<T> &&p) : m_promise(std::move(p)) {}

  bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> h) {
    auto node = _::_extract_node(std::move(m_promise));
    h.promise().m_node->template set_await<T>(std::move(node), &m_value);
  }

  T await_resume() {
    return std::move(m_value).unwrap();
  }

private:
  Promise<T> m_promise;
  Option<T>  m_value;
};

/* ── PromiseAwaiter<void> ────────────────────────────────────────── */

template <> class PromiseAwaiter<void> {
public:
  PromiseAwaiter(Promise<void> &&p) : m_promise(std::move(p)) {}

  bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> h) {
    auto node = _::_extract_node(std::move(m_promise));
    h.promise().m_node->set_await_void(std::move(node), &m_ready);
  }

  void await_resume() {}

private:
  Promise<void> m_promise;
  bool          m_ready = false;
};

/* ── operator co_await (ADL, rvalue) ─────────────────────────────── */

template <class T> PromiseAwaiter<T> operator co_await(Promise<T> &&p) {
  return PromiseAwaiter<T>(std::move(p));
}

} // namespace xpp

/* ── std::coroutine_traits specialization ────────────────────────── */

namespace std {

template <class T, class... Args> struct coroutine_traits<xpp::Promise<T>, Args...> {
  using promise_type = xpp::_::CoroutinePromise<T>;
};

} // namespace std

#endif // XPP_PROMISE_COROUTINE_H
