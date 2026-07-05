/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * notify_mt_test.cpp — Multi-threaded tests for xpp::sync::Notify.
 *
 * Requires -DXPP_MT.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/notify.h>

#include <thread>

#if !XPP_MT
// Skip tests when XPP_MT is not defined.
#else

// ── worker notifies, event loop waits ───────────────────────────────

TEST(NotifyMtTest, WorkerNotifiesLoop) {
  xpp::sync::Notify n;

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::thread worker([&n] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    n.notify_one();
  });

  int counter = 0;

  auto waiter = [&]() -> xpp::Promise<void> {
    co_await n.notified();
    counter = 1;
    co_return;
  };
  waiter().wait();
  EXPECT_EQ(counter, 1);
  worker.join();
}

// ── multiple workers notify_waiters ─────────────────────────────────

TEST(NotifyMtTest, MultipleWorkersNotifyWaiters) {
  xpp::sync::Notify n;

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::thread t1([&n] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    n.notify_waiters();
  });
  std::thread t2([&n] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    n.notify_waiters();
  });

  int c1 = 0, c2 = 0;

  auto w1 = [&]() -> xpp::Promise<void> {
    co_await n.notified();
    c1 = 1;
    co_return;
  };
  auto w2 = [&]() -> xpp::Promise<void> {
    co_await n.notified();
    c2 = 1;
    co_return;
  };

  w1().wait();
  w2().wait();
  EXPECT_EQ(c1, 1);
  EXPECT_EQ(c2, 1);
  t1.join();
  t2.join();
}

#endif // XPP_MT
