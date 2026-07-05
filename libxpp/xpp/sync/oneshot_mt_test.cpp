/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * oneshot_mt_test.cpp — Multi-threaded tests for xpp::sync::oneshot.
 *
 * oneshot wraps xpp::async<T>(), whose PromiseResolver is internally
 * thread-safe (Arc<ArcWeak> + atomic CAS). No extra mutex needed.
 *
 * Requires -DXPP_MT.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/oneshot.h>

#include <thread>

#if !XPP_MT
// Skip all tests when XPP_MT is not defined.
#else

using namespace xpp;
using namespace xpp::sync::oneshot;

// ── send from worker thread, recv on event loop ─────────────────────

TEST(OneshotMtTest, SendFromWorkerRecvOnLoop) {
  auto [tx, rx] = channel<int>();

  std::thread worker([&tx] {
    tx.send(42);
  });

  EventLoop loop;
  WaitScope scope(loop);

  auto recver = [&]() -> Promise<void> {
    int val = co_await std::move(rx).recv();
    EXPECT_EQ(val, 42);
    co_return;
  };
  recver().wait();

  worker.join();
}

// ── send on event loop, recv on worker thread ───────────────────────

TEST(OneshotMtTest, SendOnLoopRecvInWorker) {
  auto [tx, rx] = channel<int>();

  std::thread worker([&rx]() mutable {
    EventLoop loop;
    WaitScope scope(loop);

    auto recver = [&]() -> Promise<void> {
      int val = co_await std::move(rx).recv();
      EXPECT_EQ(val, 99);
      co_return;
    };
    recver().wait();
  });

  // Give worker thread a moment to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  tx.send(99);
  worker.join();
}

// ── resolve before await (value already ready) ──────────────────────

TEST(OneshotMtTest, ResolveBeforeAwait) {
  auto [tx, rx] = channel<int>();

  // send from this thread BEFORE starting the receiver
  tx.send(77);

  std::thread worker([&rx]() mutable {
    EventLoop loop;
    WaitScope scope(loop);

    auto recver = [&]() -> Promise<void> {
      int val = co_await std::move(rx).recv();
      EXPECT_EQ(val, 77);
      co_return;
    };
    recver().wait();
  });

  worker.join();
}

// ── send a move-only type across threads ────────────────────────────

TEST(OneshotMtTest, MoveOnlyType) {
  auto [tx, rx] = channel<std::unique_ptr<int>>();

  std::thread worker([&tx] {
    tx.send(std::make_unique<int>(42));
  });

  EventLoop loop;
  WaitScope scope(loop);

  auto recver = [&]() -> Promise<void> {
    auto val = co_await std::move(rx).recv();
    EXPECT_NE(val, nullptr);
    EXPECT_EQ(*val, 42);
    co_return;
  };
  recver().wait();

  worker.join();
}

#endif // XPP_MT
