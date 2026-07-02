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

/* Scenario:
 *
 *   1. Create outer promise via PromiseResolver::create() (AdapterPromiseNode)
 *      — no timer, no active source registered.
 *   2. Start a 30ms timer that resolves the inner promise.
 *   3. Start a 60ms timer that resolves the outer promise.
 *   4. outer.then() calls inner.wait() — nests xEventLoopRun.
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

  auto outer_r = xpp::PromiseResolver<int>::create();
  auto outer_p = outer_r.promise();
  auto inner_r = xpp::PromiseResolver<int>::create();
  auto inner_p = inner_r.promise();

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
                   int inner_val = inner_p.wait();
                   return outer_val + inner_val;
                 })
                 .wait();

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

  auto outer_r = xpp::PromiseResolver<int>::create();
  auto outer_p = outer_r.promise();
  auto inner_r = xpp::PromiseResolver<int>::create();
  auto inner_p = inner_r.promise();

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
                   int inner_val = inner_p.wait();
                   return outer_val + inner_val;
                 })
                 .wait();

  EXPECT_EQ(result, 49); /* 7 + 42 */

  if (timer) xTimerStop(timer);
}

/* The REAL potential deadlock:
 *
 *   - Outer promise has NO timer/source — pure AdapterPromiseNode.
 *   - Inner wait runs first (inside then), consumes the only timer.
 *   - That timer resolves BOTH inner and outer.
 *   - After inner wait returns, there are zero active sources.
 *   - Outer Run sees alive=false → exits immediately.
 *   - But outer's done flag was posted inside the inner Run's drain.
 *   - Question: does the outer Run pick up the done flag before
 *     checking alive?
 *
 * Flow:
 *   outer.wait() → poll → None → xEventLoopRun
 *     drain_done: empty (nothing posted yet)
 *     poll_io: waits for timer (30ms)
 *     drain_done: empty
 *     fire_timers: timer fires → resolves inner + outer
 *       → inner waker posts done_inner
 *       → outer waker posts done_outer
 *     alive: done_head != NULL (two posts) → true → continue
 *     drain_done: executes done_inner → sets inner's done flag
 *                 executes done_outer → sets outer's done flag
 *     ... but wait, the timer callback resolved outer which fires
 *     outer's waker which posts done_outer. This happens INSIDE
 *     fire_timers, not inside drain_done. So after fire_timers,
 *     done_head has 2 entries. alive=true. Next iteration:
 *     drain_done → executes both → done flags set.
 *     alive: done_head empty, no timer, no source → false → exit
 *   outer while(!done) → done=true → poll → Some → return.
 *
 * This should work because the done flag is set during drain_done,
 * and alive is checked AFTER drain_done.
 */

TEST(PromiseDeadlockTest, PureAdapterNoTimerNestedWait) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto outer_r = xpp::PromiseResolver<int>::create();
  auto outer_p = outer_r.promise();
  auto inner_r = xpp::PromiseResolver<int>::create();
  auto inner_p = inner_r.promise();

  /* Timer at 30ms resolves BOTH promises simultaneously */
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

  /* then() waits for inner. Both resolve at the same timer fire.
   * Inner wait runs xEventLoopRun which drains the timer.
   * After inner wait returns, outer should already be resolved
   * (same timer callback). But outer's done flag was posted
   * inside the inner Run's drain — does outer Run see it? */
  int result = outer_p
                 .then([&inner_p](int outer_val) {
                   /* By the time this runs, outer already resolved (same timer).
                    * But we need to wait for inner which also resolved.
                    * Inner wait's Run will drain the timer, resolve both,
                    * post both done flags. Inner wait picks up its flag.
                    * Then outer wait needs to pick up its flag. */
                   int inner_val = inner_p.wait();
                   return outer_val + inner_val;
                 })
                 .wait();

  EXPECT_EQ(result, 49);

  if (timer) xTimerStop(timer);
}

/* Even worse: outer promise is resolved ONLY by the inner wait's
 * completion — no timer at all. The outer promise is resolved
 * inside the inner promise's then() chain.
 *
 * Flow:
 *   1. Start timer (30ms) → resolves inner
 *   2. outer.wait() → poll → None → Run
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
  auto outer_r = xpp::PromiseResolver<int>::create();
  auto outer_p = outer_r.promise();
  auto inner_r = xpp::PromiseResolver<void>::create();
  auto inner_p = inner_r.promise();

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
  chain.wait();

  /* Now outer should be resolved. wait() should return immediately. */
  EXPECT_EQ(outer_p.wait(), 42);

  if (timer) xTimerStop(timer);
}
