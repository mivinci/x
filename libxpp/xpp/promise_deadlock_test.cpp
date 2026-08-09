/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_deadlock_test.cpp - Reproduce the nested wait() deadlock
 * scenario where the outer promise is deferred (AdapterPromiseNode)
 * and the inner wait() consumes all active sources.
 */

#include <gtest/gtest.h>
#include <xpp/promise.h>

#include <x/base/event.h>

#if XPP_FIBER
#include <chrono>
#include <thread>

#include <xpp/fiber.h>
#endif

/* Scenario:
 *
 *   1. Create outer promise via PromiseResolver::create() (AdapterPromiseNode)
 *      — no timer, no active source registered.
 *   2. Start a 30ms timer that resolves the inner promise.
 *   3. Start a 60ms timer that resolves the outer promise.
 *   4. outer.then() calls inner.await() — nests xEventLoopRun.
 *   5. Inner wait's Run consumes the 30ms timer → inner resolves.
 *   6. Inner wait returns.
 *   7. Outer Run continues → 60ms timer should fire → outer resolves.
 *
 * If the inner Run's exit causes alive=false for the outer Run,
 * the outer while(!done) spins forever because the 60ms timer
 * was consumed (or never registered properly).
 *
 * Actually the 60ms timer IS still in the heap — it wasn't consumed
 * by the inner Run (it fires at 60ms, inner Run exits at ~30ms).
 * So alive should still be true. Let's see if it actually deadlocks.
 */

TEST(PromiseDeadlockTest, NestedWaitDeferredOuter) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr_outer = xpp::async<int>();
  auto outer_p  = std::move(pr_outer.first);
  auto outer_r  = std::move(pr_outer.second);
  auto pr_inner = xpp::async<int>();
  auto inner_p  = std::move(pr_inner.first);
  auto inner_r  = std::move(pr_inner.second);

  /* Inner resolves at 30ms via timer */
  struct InnerCtx {
    xpp::PromiseResolver<int> *r;
    int                        value;
  };
  auto  *ic          = new InnerCtx{&inner_r, 100};
  xTimer inner_timer = xTimerStart(
    [](void *arg) {
      auto *c = static_cast<InnerCtx *>(arg);
      c->r->resolve(c->value);
      delete c;
    },
    ic, NULL, 30, 0);

  /* Outer resolves at 60ms via timer */
  struct OuterCtx {
    xpp::PromiseResolver<int> *r;
    int                        value;
  };
  auto  *oc          = new OuterCtx{&outer_r, 7};
  xTimer outer_timer = xTimerStart(
    [](void *arg) {
      auto *c = static_cast<OuterCtx *>(arg);
      c->r->resolve(c->value);
      delete c;
    },
    oc, NULL, 60, 0);

  /* then() callback calls inner wait() — nests Run */
  int result = outer_p
                 .then([&inner_p](int outer_val) {
                   /* Outer has resolved (60ms timer fired). Now wait for inner.
                    * But inner already resolved at 30ms — this should be immediate. */
                   int inner_val = inner_p.await();
                   return outer_val + inner_val;
                 })
                 .await();

  EXPECT_EQ(result, 107); /* 7 + 100 */

  if (inner_timer) xTimerStop(inner_timer);
  if (outer_timer) xTimerStop(outer_timer);
}

/* More interesting: outer resolves AFTER inner.
 * Outer promise has no active source. Inner wait runs first
 * (inside then), consumes the inner timer. Does the outer
 * Run see the still-pending outer timer?
 *
 * Actually in this version, outer timer fires at 60ms and
 * triggers then() which calls inner wait. By that time inner
 * already resolved at 30ms. So inner wait is immediate.
 * The real question is: does the OUTER wait's Run see the
 * 60ms timer?
 *
 * Yes — the 60ms timer was started before wait() and is in
 * the timer heap. The outer Run polls with timeout = 60ms,
 * the timer fires, resolves outer, then() runs, inner wait
 * is immediate. No deadlock here.
 *
 * The REAL deadlock scenario is:
 *   - Outer promise is NOT resolved by a timer
 *   - Outer promise is resolved by the inner wait's callback
 *   - After inner wait returns, there are NO active sources
 *   - Outer Run sees alive=false, exits
 *   - Outer done flag is not set (because the outer waker
 *     was posted but consumed by the inner Run's drain)
 */

TEST(PromiseDeadlockTest, OuterResolvedInsideNestedWait) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr_outer = xpp::async<int>();
  auto outer_p  = std::move(pr_outer.first);
  auto outer_r  = std::move(pr_outer.second);
  auto pr_inner = xpp::async<int>();
  auto inner_p  = std::move(pr_inner.first);
  auto inner_r  = std::move(pr_inner.second);

  /* Inner resolves at 30ms, AND resolves outer too */
  struct Ctx {
    xpp::PromiseResolver<int> *inner_r;
    xpp::PromiseResolver<int> *outer_r;
  };
  auto  *ctx   = new Ctx{&inner_r, &outer_r};
  xTimer timer = xTimerStart(
    [](void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      c->inner_r->resolve(42);
      c->outer_r->resolve(7);
      delete c;
    },
    ctx, NULL, 30, 0);

  /* then() calls inner wait. When inner resolves (30ms),
   * the same timer callback also resolves outer.
   *
   * Inner wait's Run drains the timer → fires callback →
   * resolves both inner and outer → posts done flags for both.
   * Inner Run's drain picks up inner's done flag → inner wait returns.
   *
   * Outer's done flag was also posted. But does the outer Run
   * pick it up? The outer Run is still on the call stack.
   * After inner wait returns, outer Run continues its loop:
   *   drain_done_queue → picks up outer's done flag → done = true
   *   alive? done queue now empty, no timer, no source → false → exit
   * Outer while(!done) → done == true → exit → poll → ready.
   *
   * This should work! Let's verify.
   */
  int result = outer_p
                 .then([&inner_p](int outer_val) {
                   int inner_val = inner_p.await();
                   return outer_val + inner_val;
                 })
                 .await();

  EXPECT_EQ(result, 49); /* 7 + 42 */

  if (timer) xTimerStop(timer);
}

/* Even worse: outer promise is resolved ONLY by the inner wait's
 * completion — no timer at all. The outer promise is resolved
 * inside the inner promise's then() chain.
 *
 * Flow:
 *   1. Start timer (30ms) → resolves inner
 *   2. outer.await() → poll → None → Run
 *      Run polls, waits for timer
 *      Timer fires → resolves inner → posts done_inner
 *      But nobody resolves outer!
 *      drain_done → done_inner set
 *      alive: done queue empty, no timer → false → Run exits
 *   3. outer while(!done) → done=false → Run again
 *      alive: nothing → false → Run exits immediately
 *   4. SPIN FOREVER — outer promise will never resolve
 *
 * This IS the real deadlock. But it's not a bug in wait() or
 * xEventLoopRun — it's a user error: the user created a promise
 * that nobody ever resolves. The same would happen with a single
 * (non-nested) wait() on a promise nobody resolves.
 *
 * So the real question is: does nested wait() introduce a NEW
 * deadlock that wouldn't happen with non-nested wait()?
 */

TEST(PromiseDeadlockTest, OuterResolvedByInnerThenCallback) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  /* Inner promise resolved by timer at 30ms.
   * Inner's then() callback resolves outer.
   * Outer's wait() drives the loop. */
  auto pr_outer = xpp::async<int>();
  auto outer_p  = std::move(pr_outer.first);
  auto outer_r  = std::move(pr_outer.second);
  auto pr_inner = xpp::async<void>();
  auto inner_p  = std::move(pr_inner.first);
  auto inner_r  = std::move(pr_inner.second);

  struct Ctx {
    xpp::PromiseResolver<void> *inner_r;
  };
  auto  *ctx   = new Ctx{&inner_r};
  xTimer timer = xTimerStart(
    [](void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      c->inner_r->resolve();
      delete c;
    },
    ctx, NULL, 30, 0);

  /* Chain: inner resolves → then() resolves outer with 42.
   * Use discard() to ignore the void result, then wait for
   * the chain to complete. */
  auto chain = inner_p.then([&outer_r]() -> int {
    outer_r.resolve(42);
    return 0;
  });
  chain.await();

  /* Now outer should be resolved. wait() should return immediately. */
  EXPECT_EQ(outer_p.await(), 42);

  if (timer) xTimerStop(timer);
}

#if XPP_FIBER

/* ═══════════════════════════════════════════════════════════════════════
 *  Fiber-mode deadlock tests
 *
 * Before the fix, same-thread fiber wake() did xFiberSwitch without
 * setting the woken flag, but park() still looped on while(!woken).
 * The fiber resumed, saw woken==false, xFiberYield'd again — wake
 * lost, deadlock.  These tests exercise the fixed protocol with
 * genuinely pending promises (timers / cross-thread resolve).
 * ═══════════════════════════════════════════════════════════════════════ */

/* Fiber parks on outer (30ms), then parks on inner (60ms).
 * Both promises are genuinely pending when awaited — the fiber
 * suspends twice via xFiberYield and is resumed twice via
 * xFiberSwitch.  Before the fix, the first resume would loop
 * forever (woken not set). */
TEST(PromiseDeadlockTest, FiberNestedAwaitBothPending) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr_outer = xpp::async<int>();
  auto outer_p  = std::move(pr_outer.first);
  auto outer_r  = std::move(pr_outer.second);
  auto pr_inner = xpp::async<int>();
  auto inner_p  = std::move(pr_inner.first);
  auto inner_r  = std::move(pr_inner.second);

  /* Outer resolves at 30ms — fiber parks, then resumes. */
  struct OuterCtx {
    xpp::PromiseResolver<int> *r;
    int                        v;
  };
  auto  *oc          = new OuterCtx{&outer_r, 7};
  xTimer outer_timer = xTimerStart(
    [](void *a) {
      auto *c = static_cast<OuterCtx *>(a);
      c->r->resolve(c->v);
      delete c;
    },
    oc, nullptr, 30, 0);

  /* Inner resolves at 60ms — fiber parks again, then resumes. */
  struct InnerCtx {
    xpp::PromiseResolver<int> *r;
    int                        v;
  };
  auto  *ic          = new InnerCtx{&inner_r, 100};
  xTimer inner_timer = xTimerStart(
    [](void *a) {
      auto *c = static_cast<InnerCtx *>(a);
      c->r->resolve(c->v);
      delete c;
    },
    ic, nullptr, 60, 0);

  auto p = xpp::fiber([&]() {
    int ov = outer_p.await(); /* park → 30ms → xFiberSwitch → resume */
    int iv = inner_p.await(); /* park → 60ms → xFiberSwitch → resume */
    return ov + iv;
  });

  EXPECT_EQ(std::move(p).await(), 107);

  if (inner_timer) xTimerStop(inner_timer);
  if (outer_timer) xTimerStop(outer_timer);
}

/* Fiber parks on outer. Single timer resolves both outer and inner
 * simultaneously. Fiber resumes, then inner.await() is immediate
 * (already resolved by the same timer fire). */
TEST(PromiseDeadlockTest, FiberNestedAwaitSimultaneousResolve) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr_outer = xpp::async<int>();
  auto outer_p  = std::move(pr_outer.first);
  auto outer_r  = std::move(pr_outer.second);
  auto pr_inner = xpp::async<int>();
  auto inner_p  = std::move(pr_inner.first);
  auto inner_r  = std::move(pr_inner.second);

  struct Ctx {
    xpp::PromiseResolver<int> *inner_r;
    xpp::PromiseResolver<int> *outer_r;
  };
  auto  *ctx   = new Ctx{&inner_r, &outer_r};
  xTimer timer = xTimerStart(
    [](void *a) {
      auto *c = static_cast<Ctx *>(a);
      c->inner_r->resolve(42);
      c->outer_r->resolve(7);
      delete c;
    },
    ctx, nullptr, 30, 0);

  auto p = xpp::fiber([&]() {
    int ov = outer_p.await(); /* park → 30ms → resume (both resolved) */
    int iv = inner_p.await(); /* already resolved — immediate */
    return ov + iv;
  });

  EXPECT_EQ(std::move(p).await(), 49);

  if (timer) xTimerStop(timer);
}

/* Cross-thread resolve wakes a parked fiber.
 *
 * A worker thread calls resolver.resolve() from a different thread.
 * PromiseWaker::wake() detects loop != xEventLoopCurrent() and posts
 * on_wake to the fiber's event loop. on_wake runs on the loop thread
 * and calls xFiberSwitch to resume the fiber.  This exercises the
 * cross-thread fiber wake path (on_wake's fiber branch). */
TEST(PromiseDeadlockTest, FiberCrossThreadResolve) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr = xpp::async<int>();
  auto p  = std::move(pr.first);
  auto r  = std::move(pr.second);

  /* Worker thread resolves after 30ms — from a different thread. */
  std::thread worker([&r]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    r.resolve(42);
  });

  auto fiber_p = xpp::fiber([&]() {
    return p.await(); /* park → cross-thread resolve → on_wake → xFiberSwitch */
  });

  EXPECT_EQ(std::move(fiber_p).await(), 42);
  worker.join();
}

#endif // XPP_FIBER
