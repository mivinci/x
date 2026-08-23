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
