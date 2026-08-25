/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * spawn.h - xpp::spawn(): drive a promise chain to completion on the
 *           event loop, without a fiber and without a blocking await.
 *
 * Mirrors tokio::spawn: the promise is polled on the event loop; whenever
 * it suspends, its waker is registered with the suspension source (e.g.
 * an mpsc channel), and every wake re-posts a poll step to the loop via
 * the waker's wake callback (see PromiseWaker::create_with_wake_cb). No
 * fiber stack, no per-iteration polling — purely waker-driven, like tokio.
 *
 * Returns a JoinHandle-style Promise that resolves with the chain's
 * result (ignore it for fire-and-forget, await it for the value):
 *
 *   auto handle = xpp::spawn(http_get("/a"));      // await for result
 *   xpp::spawn([] { use(result); });               // fire-and-forget
 *
 * Spawned chains run on the event loop that was current at spawn()
 * time (the caller must be inside a WaitScope).
 *
 * The spawned chain must eventually resolve (or its suspension source
 * must be closed) — a chain that never completes leaks its driver state,
 * as with any fire-and-forget task runtime.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_SPAWN_H
#define XPP_SPAWN_H

#include <atomic>
#include <cstddef>
#include <utility>

#include <xpp/event.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>
#include <xpp/promise_context.h>
#include <xpp/promise_waker.h>

namespace xpp {

namespace _ {

template <class T> struct SpawnState;

// One poll step, posted to the event loop. Polls the node with the
// state's waker; if the node suspends, the waker's wake callback posts
// another step when the suspension source wakes it.
template <class T>
void resolve_spawn_result(PromiseResolver<T> &r, Option<typename PromiseNode<T>::ValueType> &v) {
  r.resolve(std::move(v.unwrap()));
}

template <> inline void resolve_spawn_result<void>(PromiseResolver<void> &r, Option<Void> &v) {
  (void)v;
  r.resolve();
}

// Late/duplicate-wake safety: every posted step owns a reference to the
// SpawnState (retained before the post; released below), and completed
// chains are marked `done` — a step arriving after completion is a
// harmless no-op instead of a use-after-free. Sources that fire wakes
// repeatedly (e.g. mpsc channels) rely on this.
template <class T> void spawn_step(void *arg) {
  auto *st = static_cast<SpawnState<T> *>(arg);
  if (!st->done.load(std::memory_order_acquire)) {
    PromiseContext                             cx(st->waker);
    Option<typename PromiseNode<T>::ValueType> r = st->node->poll(cx);
    if (r.is_some()) {
      st->done.store(true, std::memory_order_release);
      resolve_spawn_result(st->resolver, r); // fulfill the JoinHandle
      st->waker.reset();                     // break the st→waker→st ownership cycle
    }
    // else: suspended; the waker (registered by the suspension source)
    // will re-post spawn_step on wake.
  }
  st->release(); // this step's reference (may delete st)
}

template <class T> struct SpawnState {
  OwnPromiseNode<T>     node;
  PromiseWaker          waker;
  PromiseResolver<T>    resolver;
  std::atomic<bool>     done{false};
  std::atomic<uint32_t> refs{0};

  SpawnState(OwnPromiseNode<T> n, PromiseWaker w, PromiseResolver<T> r)
      : node(std::move(n)), waker(std::move(w)), resolver(std::move(r)) {}

  void retain() {
    refs.fetch_add(1, std::memory_order_relaxed);
  }
  void release() {
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }

  static void on_retain(void *p) {
    static_cast<SpawnState *>(p)->retain();
  }
  static void on_release(void *p) {
    static_cast<SpawnState *>(p)->release();
  }
};

template <class T> Promise<T> spawn_impl(Promise<T> &&p) {
  auto  out      = async<T>();
  auto  promise  = std::move(out.first);
  auto  resolver = std::move(out.second);
  auto *st       = new SpawnState<T>(_extract_node(std::move(p)),
                                     PromiseWaker::create_with_wake_cb(&spawn_step<T>, nullptr),
                                     std::move(resolver));
  // The waker owns one reference; the initial posted step owns another.
  st->waker.attach_wake_arg(st, &SpawnState<T>::on_retain, &SpawnState<T>::on_release);
  st->retain();
  xEventLoopPost(xEventLoopCurrent(), &spawn_step<T>, st);
  return promise;
}

} // namespace _

/**
 * @brief Drive a promise chain to completion on the event loop.
 *
 * The chain is polled on the current event loop (the caller must be
 * inside a WaitScope). When the chain suspends (e.g. awaiting an mpsc
 * channel), its waker is registered with the suspension source; every
 * wake re-posts a poll step, so the chain progresses without a fiber
 * and without a blocking await loop. The final value is discarded.
 *
 * The chain must be self-contained — attach side effects with .then().
 */
template <class T> Promise<T> spawn(Promise<T> p) {
  return _::spawn_impl(std::move(p));
}

/**
 * @brief Run a callable as a fire-and-forget task on the event loop.
 *
 * Convenience overload mirroring tokio::spawn(async { ... }): the
 * callable is wrapped via xpp::defer (its return value is flattened if
 * it returns a Promise). The callable runs on the event loop; to await
 * inside it, return a Promise chain (`.then(...)`) rather than calling
 * `.await()` (which would block unless already inside a fiber).
 *
 * @par Coroutine lambdas and closure lifetime
 * A lambda coroutine's frame stores the closure *pointer* (`this`),
 * not a copy of the closure object. Passing a coroutine lambda directly
 * to spawn() is safe — the defer node keeps a heap copy of the closure
 * for the chain's lifetime. But the pattern
 * @code
 *   auto make = [&]() -> Promise<void> { ... };
 *   xpp::spawn(make());
 * @endcode
 * requires `make` to outlive the spawned chain: if the closure lives on
 * a stack frame that dies before the chain is polled (e.g. an outer
 * plain lambda running inside a poll callback), the deferred poll
 * resumes the coroutine through a dangling closure pointer and crashes.
 * Prefer passing the lambda itself, or use a named coroutine function
 * (whose arguments are copied into the frame).
 */
template <class Func> auto spawn(Func &&fn) {
  return spawn(xpp::defer(std::forward<Func>(fn)));
}

} // namespace xpp

#endif // XPP_SPAWN_H
