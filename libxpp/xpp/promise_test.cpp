/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_test.cpp - Tests for xpp::Promise
 *
 * All tests run inside a WaitScope — no runtime layer needed.
 */

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/promise_test_helper.h>

#include <x/base/event.h>

/* ───────────────────── Immediate resolve ───────────────────── */

TEST(PromiseTest, ResolveImmediateInt) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::resolve(42);
  EXPECT_EQ(p.wait(), 42);
}

TEST(PromiseTest, ResolveImmediateVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::yield();
  p.wait();
  SUCCEED();
}

TEST(PromiseTest, ResolveImmediateString) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::resolve(std::string("hello"));
  EXPECT_EQ(p.wait(), "hello");
}

/* ───────────────────── then() chain ───────────────────── */

TEST(PromiseTest, ThenTransformInt) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::resolve(1).then([](int x) { return x + 1; }).wait();
  EXPECT_EQ(result, 2);
}

TEST(PromiseTest, ThenChainedTransforms) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::resolve(10)
                 .then([](int x) { return x * 2; })
                 .then([](int x) { return x - 5; })
                 .wait();
  EXPECT_EQ(result, 15);
}

TEST(PromiseTest, ThenVoidToInt) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::yield().then([]() { return 42; }).wait();
  EXPECT_EQ(result, 42);
}

TEST(PromiseTest, ThenIntToVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::resolve(42).then([](int) {}).wait();
  SUCCEED();
}

TEST(PromiseTest, ThenReturnsPromise) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::resolve(1)
                 .then([](int x) { return xpp::resolve(x * 10); })
                 .wait();
  EXPECT_EQ(result, 10);
}

/* ───────────────────── Deferred resolve ───────────────────── */

TEST(PromiseTest, DeferredResolveInt) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  xpp::Timer t = schedule_resolve(r, 99, 50);

  EXPECT_EQ(p.wait(), 99);
}

TEST(PromiseTest, DeferredResolveVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<void>();
  xpp::Timer t = schedule_resolve(r, 50);

  p.wait();
}

TEST(PromiseTest, DeferredResolveWithThen) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  xpp::Timer t = schedule_resolve(r, 7, 50);

  int result = p.then([](int x) { return x * 3; }).wait();
  EXPECT_EQ(result, 21);
}

/* ───────────────────── defer / yield ───────────────────── */

TEST(PromiseTest, EvalSynchronous) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::defer([] { return 42; }).wait();
  EXPECT_EQ(result, 42);
}

TEST(PromiseTest, YieldThenTransform) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::yield().then([] { return 1; }).wait();
  EXPECT_EQ(result, 1);
}

/* ───────────────────── discard ───────────────────── */

TEST(PromiseTest, DiscardValue) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::resolve(42).discard().wait();
  SUCCEED();
}

/* ───────────────────── empty promise ───────────────────── */

TEST(PromiseTest, EmptyPromiseAsserts) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::Promise<int> empty;
  EXPECT_FALSE(empty);
}

/* ───────────────────── Resolver ───────────────────── */

TEST(PromiseTest, ResolverIsPendingBeforeResolve) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  EXPECT_TRUE(r.is_pending());
  r.resolve(1);
  EXPECT_FALSE(r.is_pending());
  EXPECT_EQ(p.wait(), 1);
}

TEST(PromiseTest, ResolveBeforePollIsImmediate) {
  /* resolve() before wait() — poll should see ready immediately,
   * no event loop iteration needed. */
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  r.resolve(77);
  EXPECT_EQ(p.wait(), 77);
}

TEST(PromiseTest, ResolveVoidBeforePoll) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<void>();
  r.resolve();
  p.wait();
  SUCCEED();
}

/* ───────────────────── void → void then ───────────────────── */

TEST(PromiseTest, ThenVoidToVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  bool called = false;
  xpp::yield().then([&called]() { called = true; }).wait();
  EXPECT_TRUE(called);
}

TEST(PromiseTest, ThenVoidToVoidChained) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int counter = 0;
  xpp::yield()
    .then([&counter]() { counter++; })
    .then([&counter]() { counter++; })
    .then([&counter]() { counter++; })
    .wait();
  EXPECT_EQ(counter, 3);
}

/* ───────────────────── Promise move ───────────────────── */

TEST(PromiseTest, MovePromise) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto              p1 = xpp::resolve(5);
  xpp::Promise<int> p2 = std::move(p1);
  EXPECT_FALSE(p1);
  EXPECT_TRUE(p2);
  EXPECT_EQ(p2.wait(), 5);
}

/* ───────────────────── Recursive wait ───────────────────── */

/**
 * wait() inside a callback that runs during another wait().
 * This exercises nested xEventLoopRun — previously broken because
 * xEventLoopRun would Leave on the inner return, unbinding the loop.
 * Now that xEventLoopRun no longer calls Enter/Leave, nesting is safe.
 */

TEST(PromiseTest, NestedWaitImmediate) {
  /* Outer promise resolves immediately, then() callback calls
   * an inner wait() on another immediately-resolved promise. */
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::resolve(1)
                 .then([](int x) {
                   /* Inner wait — runs inside the outer then() chain.
                    * The outer wait() drives the loop; this inner wait() runs
                    * during a done-queue drain, which means xEventLoopRun is
                    * already on the call stack. The inner wait() calls
                    * xEventLoopRun again — nested. */
                   return xpp::resolve(x * 100).wait();
                 })
                 .wait();

  EXPECT_EQ(result, 100);
}

TEST(PromiseTest, NestedWaitDeferred) {
  /* Both outer and inner are deferred — the inner timer fires
   * while the outer wait's xEventLoopRun is running. */
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr_outer = xpp::async<int>();
  auto outer_p  = std::move(pr_outer.first);
  auto pr_inner = xpp::async<int>();
  auto inner_p  = std::move(pr_inner.first);

  /* Schedule outer resolve at 30ms — triggers then() which calls
   * inner wait() on a not-yet-resolved inner promise. */
  xpp::Timer outer_timer = xpp::schedule_resolve(pr_outer.second, 7, 30);

  /* Schedule inner resolve at 60ms — fires while inner wait()'s
   * xEventLoopRun is running. */
  xpp::Timer inner_timer = xpp::schedule_resolve(pr_inner.second, 42, 60);

  /* then() callback calls inner wait() — this nests xEventLoopRun. */
  int result = outer_p
                 .then([&inner_p](int outer_val) {
                   /* This blocks until inner resolves (at 60ms).
                    * The outer timer (30ms) has already fired and we're inside
                    * the outer wait's xEventLoopRun. This inner wait() starts
                    * a nested xEventLoopRun. */
                   int inner_val = inner_p.wait();
                   return outer_val + inner_val;
                 })
                 .wait();

  EXPECT_EQ(result, 49); /* 7 + 42 */

}

TEST(PromiseTest, TripleNestedWait) {
  /* Three levels of nesting: wait → then → wait → then → wait. */
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto  pr3 = xpp::async<int>();
  auto  p3  = std::move(pr3.first);
  auto &r3  = pr3.second;

  /* Resolve p3 at 30ms */
  xpp::Timer t3 = xpp::schedule_resolve(r3, 300, 30);

  int result = xpp::resolve(1)
                 .then([&p3](int a) {
                   /* Level 2: immediately resolved, but wait() still nests Run */
                   return xpp::resolve(a * 2)
                     .then([&p3](int b) {
                       /* Level 3: deferred — wait() nests Run inside Run inside Run */
                       return b + p3.wait();
                     })
                     .wait();
                 })
                 .wait();

  EXPECT_EQ(result, 302); /* (1*2) + 300 */

}

/* ───────────────────── Cross-thread resolve ───────────────────── */

/**
 * Resolver::resolve() is thread-safe — may be called from any thread.
 * These tests verify that a worker thread can resolve a promise while
 * the WaitScope thread blocks in wait().
 */

TEST(PromiseTest, CrossThreadResolveInt) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr = xpp::async<int>();
  auto r  = std::move(pr.second);

  std::thread worker([&r]() {
    /* Simulate async work — resolve from another thread */
    r.resolve(77);
  });

  EXPECT_EQ(pr.first.wait(), 77);
  worker.join();
}

TEST(PromiseTest, CrossThreadResolveVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr = xpp::async<void>();
  auto r  = std::move(pr.second);

  std::thread worker([&r]() { r.resolve(); });

  pr.first.wait();
  worker.join();
  SUCCEED();
}

TEST(PromiseTest, CrossThreadResolveString) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr = xpp::async<std::string>();
  auto r  = std::move(pr.second);

  std::thread worker([&r]() { r.resolve(std::string("from another thread")); });

  EXPECT_EQ(pr.first.wait(), "from another thread");
  worker.join();
}

TEST(PromiseTest, CrossThreadResolveWithThen) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr = xpp::async<int>();
  auto r  = std::move(pr.second);

  std::thread worker([&r]() { r.resolve(21); });

  int result = pr.first.then([](int x) { return x * 2; }).wait();
  EXPECT_EQ(result, 42);
  worker.join();
}

TEST(PromiseTest, CrossThreadResolveDelayed) {
  /* Worker thread sleeps before resolving — verifies that wait()
   * blocks and the event loop runs while waiting. */
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr = xpp::async<int>();
  auto r  = std::move(pr.second);

  std::thread worker([&r]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.resolve(99);
  });

  EXPECT_EQ(pr.first.wait(), 99);
  worker.join();
}

TEST(PromiseTest, CrossThreadResolveBeforePoll) {
  /* Worker resolves before wait() is even called — poll should see
   * the value immediately without running the event loop. */
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto pr = xpp::async<int>();
  auto r  = std::move(pr.second);

  std::thread worker([&r]() { r.resolve(42); });
  worker.join();

  /* Now wait — promise is already resolved. */
  EXPECT_EQ(pr.first.wait(), 42);
}

/* ───────────────────── after() — timer-based deferred promise ───────────────────── */

TEST(PromiseTest, AfterZeroDelayResolves) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  bool fired = false;
  xpp::after(0).then([&]() { fired = true; }).wait();
  EXPECT_TRUE(fired);
}

TEST(PromiseTest, AfterApproximateDelay) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t0 = std::chrono::steady_clock::now();
  xpp::after(30).wait();
  auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
      .count();

  EXPECT_GE(ms, 25) << "should wait at least ~25ms, got " << ms;
  EXPECT_LE(ms, 500) << "should not take too long, got " << ms;
}

TEST(PromiseTest, AfterThenVoidChain) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int step = 0;
  xpp::after(10).then([&]() { step = 1; }).then([&]() { step = 2; }).wait();

  EXPECT_EQ(step, 2);
}

TEST(PromiseTest, AfterThenReturnValue) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::after(10).then([]() -> int { return 42; }).wait();
  EXPECT_EQ(result, 42);
}

TEST(PromiseTest, AfterComposeWithImmediatePromise) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::after(10)
                 .then([]() { return xpp::resolve(7); })
                 .then([](int x) { return x * 6; })
                 .wait();

  EXPECT_EQ(result, 42);
}

TEST(PromiseTest, AfterSequentialWaitsInOrder) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> order;

  xpp::after(10).then([&]() { order.push_back(0); }).wait();
  xpp::after(10).then([&]() { order.push_back(1); }).wait();
  xpp::after(10).then([&]() { order.push_back(2); }).wait();

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 0);
  EXPECT_EQ(order[1], 1);
  EXPECT_EQ(order[2], 2);
}

TEST(PromiseTest, AfterNestedAfter) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t0 = std::chrono::steady_clock::now();
  xpp::after(10).then([]() { return xpp::after(20); }).wait();

  auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
      .count();
  EXPECT_GE(ms, 25) << "nested 10+20ms should take at least ~25ms, got " << ms;
}

TEST(PromiseTest, AfterIndependentTimersAllFire) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int  a = 0, b = 0;
  auto pa = xpp::after(10).then([&]() { a = 42; });
  auto pb = xpp::after(20).then([&]() { b = 99; });

  pa.wait();
  pb.wait();

  EXPECT_EQ(a, 42);
  EXPECT_EQ(b, 99);
}

/* ───────────────────── after() — lifecycle safety tests ───────────────────── */

TEST(PromiseTest, AfterDestroyedBeforeFire) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  /* Construct a promise that won't fire for a long time, then drop it
   * immediately. ~TimerPromiseNode should call xTimerStop and no
   * use-after-free should occur. Pumping the loop briefly should not
   * crash. */
  {
    auto p = xpp::after(60000);
    (void)p;
  } // ← ~Promise here

  /* Run the loop briefly to confirm no deferred callback fires. */
  loop.run(xpp::RunMode::NoWait);
}

TEST(PromiseTest, AfterLoopDestroyedBeforeFire) {
  /* Construct a promise with a long delay, then destroy the loop
   * without firing. The on_cancel_cb should mark the node as fired
   * and null m_handle. Then dropping the promise should not crash. */
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  {
    auto p = xpp::after(60000);
    (void)p;
  } // Drop the promise first — ~TimerPromiseNode calls xTimerStop

  /* If we hadn't dropped the promise, destroying the loop would
   * trigger on_cancel_cb. Test that variant too: */
  {
    xpp::EventLoop loop2;
    xpp::WaitScope scope2(loop2);
    auto           p = xpp::after(60000);
    (void)p;
    /* ~WaitScope leaves; ~EventLoop destroys loop; on_cancel_cb fires */
  }
}

TEST(PromiseTest, AfterStillResolvesOnFire) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  /* Regression: the happy path must still work. */
  bool fired = false;
  xpp::after(10).then([&]() { fired = true; }).wait();
  EXPECT_TRUE(fired);
}

TEST(PromiseTest, AfterThenChain) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  /* Composition with then() must still work. */
  int result = xpp::after(10)
                 .then([]() { return 42; })
                 .then([](int x) { return x * 2; })
                 .wait();
  EXPECT_EQ(result, 84);
}
