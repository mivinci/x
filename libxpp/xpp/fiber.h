/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber.h - xpp::fiber() — start a Promise-driven fiber (stackful coroutine).
 *
 * The single public API: xpp::fiber(stack_size, func).
 * Creates a fiber that runs func() on its own stack. Inside the fiber,
 * Promise::wait() transparently suspends instead of blocking the event
 * loop. The fiber resumes when the promise resolves.
 *
 * Returns a Promise<T> where T = decltype(func()). The promise resolves
 * when func() returns.
 *
 * Usage:
 *
 *   auto p = xpp::fiber(65536, []() {
 *     auto a = http_get("/a").await();  // suspends fiber, not the event loop
 *     auto b = http_get("/b").await();
 *     return a + b;
 *   });
 *   // p is a Promise<int> — .then() / .await() as usual
 *
 *   loop.run();  // drives all I/O and fiber resumes
 *
 * This module requires XPP_FIBER (CMake option, links xbase for xFiber* API).
 * Without XPP_FIBER this header is empty — Promise::wait() uses the
 * blocking path exclusively.
 *
 * C++11-compatible (requires XPP_FIBER at compile time).
 */

#ifndef XPP_FIBER_H
#define XPP_FIBER_H

#include <utility>

#include <xpp/promise.h>       // Promise<T>, async(), PromiseResolver<T>
#include <xpp/promise_node.h>  // _::FixVoid
#include <xpp/promise_types.h> // _::fiber::Context

#if XPP_FIBER
#include <x/base/event.h>
#include <x/base/fiber.h> // xFiber*, xEventLoopPost, xEventLoopCurrent
#endif

namespace xpp {

#if XPP_FIBER

/* ── Internal: fiber trampoline + cleanup ─────────────────────────── */

namespace _ {
namespace fiber {

/**
 * @brief Opaque context header placed before user state in the
 *        single-allocation block (Context + user State).
 *
 * Defined in <xpp/promise_types.h> and shared with PromiseContext.
 */
// _::fiber::Context is in <xpp/promise_types.h>

/**
 * @brief Forward declaration — cleanup_cb runs on event loop to destroy fiber.
 */
static void cleanup_cb(void *arg);

/**
 * @brief Fiber entry point — called on the fiber's stack.
 *
 * 1. Calls ctx->run(ctx + 1) to execute the user's lambda
 * 2. Posts cleanup_cb to the event loop (can't destroy fiber from within)
 * 3. Switches back to the parent fiber (or main if no parent)
 */
static void trampoline(void *arg) {
  auto *ctx  = static_cast<Context *>(arg);
  void *data = ctx + 1; // user state follows Context in memory
  ctx->run(data);

  // Post cleanup to the event loop boundary.  The trampoline cannot
  // call xFiberDestroy on itself, so deferred cleanup is necessary.
  xEventLoopPost(xEventLoopCurrent(), &cleanup_cb, ctx);
  xFiberYield();
}

/**
 * @brief Runs on the event loop (main stack), destroys the fiber + user state.
 */
static void cleanup_cb(void *arg) {
  auto *ctx  = static_cast<Context *>(arg);
  void *data = ctx + 1;
  ctx->destroy(data);
  xFiberDestroy(ctx->handle);
  ctx->~Context(); // run Arc destructor (decrements refcount)
  operator delete(ctx);
}

} // namespace fiber
} // namespace _

/* ── Internal: resolve helper (handles void vs T) ────────────────── */

namespace _ {
namespace fiber {

/**
 * @brief Call func() then resolve the promise.
 *
 * For T != void: resolver.resolve(func())
 * For T == void: func(); resolver.resolve()
 *
 * Partial specialisation of function templates is not allowed in C++,
 * so we use a struct with a static method.
 */
template <typename T> struct Runner {
  template <typename Resolver, typename Func> static void run(Resolver &resolver, Func &func) {
    resolver.resolve(func());
  }
};

template <> struct Runner<void> {
  template <typename Resolver, typename Func> static void run(Resolver &resolver, Func &func) {
    func();
    resolver.resolve();
  }
};

} // namespace fiber
} // namespace _

/* ── xpp::fiber() ─────────────────────────────────────────────────── */

/**
 * @brief Start a Promise-driven fiber with a custom stack size.
 *
 * @param stack_size  Stack size in bytes. 0 uses the default (64 KiB).
 * @param func        Callable to run on the fiber's stack.
 *                    Signature: T func() or void func().
 * @return Promise<T> that resolves when func() returns.
 *
 * The fiber starts executing immediately (before this function returns).
 * If func() calls Promise::wait() on a pending promise, the fiber
 * suspends via xFiberSwitch and the event loop keeps running.  When
 * the promise resolves, the waker switches the fiber back in and
 * execution resumes after .await().
 *
 * The returned Promise is safe to use like any other Promise:
 * chain with .then(), race(), or .await().
 */
template <typename Func>
auto fiber(size_t stack_size, Func &&func) -> Promise<decltype(std::declval<Func>()())> {
  using T = decltype(std::declval<Func>()());

  auto pair     = async<T>();
  auto promise  = std::move(pair.first);
  auto resolver = std::move(pair.second);

  // User state — lambda + resolver, destroyed by Context::destroy
  struct State {
    typename std::decay<Func>::type func;
    decltype(pair.second)           resolver;
  };

  // Allocate Context + State in one block
  size_t total = sizeof(_::fiber::Context) + sizeof(State);
  void  *mem   = operator new(total);
  auto  *ctx   = static_cast<_::fiber::Context *>(mem);
  auto  *state = new (ctx + 1) State{std::forward<Func>(func), std::move(resolver)};

  // Non-capturing lambdas → void(*)(void*)
  ctx->run = [](void *s) {
    auto *st = static_cast<State *>(s);
    _::fiber::Runner<T>::run(st->resolver, st->func);
  };

  ctx->destroy = [](void *s) { static_cast<State *>(s)->~State(); };

  // Create fiber on a new stack
  ctx->handle = xFiberCreate(stack_size, &_::fiber::trampoline, ctx);
  if (!ctx->handle) {
    ctx->destroy(state);
    operator delete(mem);
    return Promise<T>(); // empty promise — caller should handle
  }

  // Per-fiber waker — 1 heap alloc for the lifetime of this fiber.
  // Every await() inside the fiber clones this Arc (1 fetch_add, 0 alloc).
  // Use placement new because ctx was allocated with operator new —
  // the Arc member's m_inner is uninitialized and operator=(Arc&&)
  // reads this->m_inner inside swap, which is UB on garbage.
  new (&ctx->waker) Arc<_::WakerCore>(Arc<_::WakerCore>::make(xEventLoopCurrent(), ctx->handle));

  // Ensure the thread is fiber-capable (idempotent), then enter the fiber.
  xFiberMain();
  xFiberSwitch(ctx->handle);
  // Back here: fiber either finished (func() returned) or suspended
  // on its first .await() call.  In both cases the Promise is live —
  // resolved or pending.

  return std::move(promise);
}

/**
 * @brief Start a Promise-driven fiber with the default stack size (64 KiB).
 */
template <typename Func> auto fiber(Func &&func) -> Promise<decltype(std::declval<Func>()())> {
  return fiber(0, std::forward<Func>(func));
}

#endif // XPP_FIBER

} // namespace xpp

#endif // XPP_FIBER_H
