/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * spawn_test.cpp — xpp::spawn(): waker-driven promise chains without
 * fibers. Primary coverage uses the tokio-style lambda overload; a few
 * tests exercise the raw Promise overload directly.
 */

#include <atomic>
#include <thread>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/promise.h>
#include <xpp/spawn.h>
#include <xpp/sync/mpsc.h>

using namespace xpp;

/* ───────────────────────────────────────────────────────────────────
 *  Synchronous lambda — runs on the first poll step
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, SyncLambda) {
  EventLoop loop;
  WaitScope scope(loop);

  std::atomic<bool> done{false};
  spawn([&done] { done.store(true, std::memory_order_release); });

  for (int i = 0; i < 100 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

/* ───────────────────────────────────────────────────────────────────
 *  Lambda returning a chain that suspends on a channel, then completes
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, LambdaWithChannelWake) {
  EventLoop loop;
  WaitScope scope(loop);

  auto [tx, rx] = sync::mpsc::channel<int>(8);
  std::atomic<bool> done{false};

  spawn([&rx, &done]() -> Promise<void> {
    return rx.recv().then([&done](Option<int> v) {
      EXPECT_TRUE(v.is_some());
      EXPECT_EQ(v.unwrap(), 42);
      done.store(true, std::memory_order_release);
    });
  });

  tx.try_send(42);
  tx.close();

  for (int i = 0; i < 1000 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire)) << "spawned lambda did not complete";
}

/* ───────────────────────────────────────────────────────────────────
 *  Lambda with two suspensions (recv twice, chained)
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, LambdaMultipleSuspensions) {
  EventLoop loop;
  WaitScope scope(loop);

  auto [tx, rx] = sync::mpsc::channel<int>(8);
  std::atomic<bool> done{false};

  spawn([&rx, &done]() -> Promise<void> {
    return rx.recv()
      .then([&rx](Option<int> a) -> Promise<int> {
        EXPECT_EQ(a.unwrap(), 1);
        return rx.recv().then([](Option<int> b) { return b.unwrap(); });
      })
      .then([&done](int v) {
        EXPECT_EQ(v, 2);
        done.store(true, std::memory_order_release);
      });
  });

  tx.try_send(1);
  tx.try_send(2);
  tx.close();

  for (int i = 0; i < 2000 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

/* ───────────────────────────────────────────────────────────────────
 *  Wake from another thread (cross-thread waker path)
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, LambdaCrossThreadWake) {
  EventLoop loop;
  WaitScope scope(loop);

  auto [tx, rx] = sync::mpsc::channel<int>(8);
  std::atomic<bool> done{false};

  spawn([&rx, &done]() -> Promise<void> {
    return rx.recv().then([&done](Option<int> v) {
      EXPECT_TRUE(v.is_some());
      done.store(true, std::memory_order_release);
    });
  });

  std::thread t([&tx] { tx.try_send(99); });
  t.join();

  for (int i = 0; i < 1000 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

/* ───────────────────────────────────────────────────────────────────
 *  Raw Promise overload (spawn(promise) directly)
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, RawPromiseOverload) {
  EventLoop loop;
  WaitScope scope(loop);

  auto [tx, rx] = sync::mpsc::channel<int>(8);
  std::atomic<bool> done{false};

  auto p_ = rx.recv().then([&done](Option<int> v) {
    EXPECT_EQ(v.unwrap(), 7);
    done.store(true, std::memory_order_release);
  });
  spawn(std::move(p_));

  tx.try_send(7);
  tx.close();

  for (int i = 0; i < 1000 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

/* ───────────────────────────────────────────────────────────────────
 *  Overload resolution: Promise value picks the Promise overload
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, PromiseOverloadResolution) {
  EventLoop loop;
  WaitScope scope(loop);

  std::atomic<bool> done{false};
  spawn(xpp::resolve(3).then([&done](int v) {
    EXPECT_EQ(v, 3);
    done.store(true, std::memory_order_release);
  }));

  for (int i = 0; i < 100 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

/* ───────────────────────────────────────────────────────────────────
 *  JoinHandle: await the returned Promise for the chain's result
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, JoinHandleAwaitResult) {
  EventLoop loop;
  WaitScope scope(loop);

  // Spawn a chain that computes a value; await the returned Promise.
  auto handle =
    spawn([]() -> Promise<int> { return xpp::resolve(21).then([](int v) { return v * 2; }); });
  EXPECT_EQ(handle.await(), 42);

  // Same via the raw Promise overload.
  auto h2 = spawn(xpp::resolve(10).then([](int v) { return v + 5; }));
  EXPECT_EQ(h2.await(), 15);
}

TEST(SpawnTest, JoinHandleVoid) {
  EventLoop loop;
  WaitScope scope(loop);

  std::atomic<bool> ran{false};
  auto              handle = spawn([&ran] { ran.store(true, std::memory_order_release); });
  handle.await();
  EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

#if XPP_HAS_COROUTINES

/* ───────────────────────────────────────────────────────────────────
 *  Nested spawn of a capturing-lambda coroutine (regression).
 *
 *  A lambda coroutine's frame stores the *closure pointer* (`this`),
 *  not a copy of the closure — the closure object must therefore
 *  outlive the spawned chain (issues/coro-nested-spawn-capture-lambda-crash.md).
 *
 *  Safe: passing the lambda directly to spawn() — the defer node keeps
 *  a heap copy of the closure for the chain's lifetime — or keeping the
 *  closure alive where it is declared (as below).
 *  Unsafe: `auto make = [&]{...}; spawn(make());` where `make` lives on
 *  a stack frame that dies before the chain is polled (e.g. a plain
 *  outer lambda running inside a poll callback): the deferred
 *  spawn_step resumes the coroutine after that frame is gone and reads
 *  the capture through a dangling closure pointer.
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, NestedSpawnCaptureLambdaCoroutine) {
  EventLoop loop;
  WaitScope scope(loop);

  bool ran       = false;
  bool inner_ran = false;

  // The closure lives in this frame, which outlives the loop run below.
  auto make = [&inner_ran]() -> Promise<void> {
    inner_ran = true;
    co_return;
  };

  // Outer chain: a plain lambda running inside a poll callback spawns
  // the capturing-lambda coroutine — nested spawn via xEventLoopPost.
  spawn([&ran, &make]() -> Promise<void> {
    spawn(make());
    ran = true;
    return xpp::resolve();
  });

  for (int i = 0; i < 100 && !(ran && inner_ran); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(ran);
  EXPECT_TRUE(inner_ran);
}

TEST(SpawnTest, SpawnedLambdaCoroutineClosureSafe) {
  EventLoop loop;
  WaitScope scope(loop);

  // Passing the coroutine lambda directly to spawn(): the closure is
  // copied into the defer node (heap) and stays alive for the chain.
  bool ran = false;
  spawn([&ran]() -> Promise<void> {
    ran = true;
    co_return;
  });

  for (int i = 0; i < 100 && !ran; ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(ran);
}

#endif // XPP_HAS_COROUTINES

/* ───────────────────────────────────────────────────────────────────
 *  .await() inside a spawned fn.
 *
 *  Verified semantics: .await() parks the spawn step's C stack and
 *  re-enters the event loop (xEventLoopRun(X_RUN_ONCE) in park()), so
 *  the world keeps turning — timers fire, other chains progress —
 *  while the fn waits. This is the re-entrant suspension model:
 *  usable, but the idiomatic forms remain .then() chains (C++11) and
 *  co_await (C++20), which are waker-driven with no parked stack.
 *
 *  Known deadlock (documented, not tested here — it hangs): awaiting
 *  the spawn's OWN JoinHandle, or anything that only resolves after
 *  this fn returns.
 * ─────────────────────────────────────────────────────────────────── */

TEST(SpawnTest, AwaitInsideSpawnedFn) {
  EventLoop loop;
  WaitScope scope(loop);

  auto [tx, rx] = sync::mpsc::channel<int>(4);
  std::atomic<bool> done{false};

  spawn([&rx, &done]() -> Promise<void> {
    auto v = rx.recv().await(); // parks the step; the loop keeps running
    EXPECT_TRUE(v.is_some());
    EXPECT_EQ(v.unwrap(), 42);
    done.store(true, std::memory_order_release);
    return xpp::resolve();
  });

  // Feed the channel from a timer while the fn is parked.
  xTimerStart([](void *arg) { static_cast<sync::mpsc::Sender<int> *>(arg)->try_send(42); }, &tx,
              NULL, 10, 0);

  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  tx.close();
  EXPECT_TRUE(done.load(std::memory_order_acquire)) << ".await() inside spawn never resolved";
}

TEST(SpawnTest, SequentialAwaitsInsideSpawnedFn) {
  EventLoop loop;
  WaitScope scope(loop);

  auto [tx, rx] = sync::mpsc::channel<int>(4);
  std::atomic<int> sum{0};

  spawn([&rx, &sum]() -> Promise<void> {
    // Two park/resume cycles in one fn — each .await() parks, gets fed
    // by a timer, resumes, and the next .await() parks again.
    auto a = rx.recv().await();
    auto b = rx.recv().await();
    sum.store(a.unwrap() + b.unwrap(), std::memory_order_release);
    return xpp::resolve();
  });

  xTimerStart([](void *arg) { static_cast<sync::mpsc::Sender<int> *>(arg)->try_send(10); }, &tx,
              NULL, 10, 0);
  xTimerStart([](void *arg) { static_cast<sync::mpsc::Sender<int> *>(arg)->try_send(32); }, &tx,
              NULL, 30, 0);

  for (int i = 0; i < 300 && sum.load(std::memory_order_acquire) == 0; ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  tx.close();
  EXPECT_EQ(sum.load(std::memory_order_acquire), 42);
}

TEST(SpawnTest, AwaitTimerInsideSpawnedFn) {
  EventLoop loop;
  WaitScope scope(loop);

  std::atomic<bool> done{false};

  spawn([&done]() -> Promise<void> {
    xpp::after(10).await(); // await a timer promise directly
    done.store(true, std::memory_order_release);
    return xpp::resolve();
  });

  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

TEST(SpawnTest, AwaitCrossChainInsideSpawnedFn) {
  EventLoop loop;
  WaitScope scope(loop);

  // Chain A awaits a promise that chain B resolves — cross-chain .await()
  // with both chains spawned on the same loop.
  auto              pr       = xpp::async<int>();
  auto              resolver = std::move(pr.second);
  std::atomic<bool> done{false};

  spawn([&done, p = std::move(pr.first)]() mutable -> Promise<void> {
    int v = p.await(); // parks until the OTHER spawn resolves it
    EXPECT_EQ(v, 7);
    done.store(true, std::memory_order_release);
    return xpp::resolve();
  });

  // C++11 lambdas cannot move-capture from an enclosing capture — hold the
  // move-only resolver on the heap and share it into the .then continuation.
  auto r_holder = Arc<PromiseResolver<int>>::make(std::move(resolver));
  spawn([r_holder]() -> Promise<void> {
    return xpp::after(10).then([r_holder]() { r_holder->resolve(7); });
  });

  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  }
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}
