/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * notify_test.cpp — Tests for xpp::sync::Notify.
 */
#include <thread>

#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/notify.h>

// ── N-1: basic notify_one → notified resolves ──────────────────────

TEST(NotifyTest, NotifyOneBasic) {
  xpp::EventLoop    loop;
  xpp::WaitScope    scope(loop);
  xpp::sync::Notify n;

  auto p = n.notified();
  n.notify_one();
  p.await(); // should complete
}

// ── N-2: notify_waiters wakes all ───────────────────────────────────

TEST(NotifyTest, NotifyWaiters) {
  xpp::EventLoop    loop;
  xpp::WaitScope    scope(loop);
  xpp::sync::Notify n;

  auto p1 = n.notified();
  auto p2 = n.notified();
  n.notify_waiters();
  p1.await();
  p2.await();
}

// ── N-3: reusable ───────────────────────────────────────────────────

TEST(NotifyTest, Reusable) {
  xpp::EventLoop    loop;
  xpp::WaitScope    scope(loop);
  xpp::sync::Notify n;

  auto p1 = n.notified();
  n.notify_one();
  p1.await();

  auto p2 = n.notified();
  n.notify_one();
  p2.await();
}

// ── N-4: notify before notified is no-op ────────────────────────────

TEST(NotifyTest, NotifyBeforeNotified) {
  xpp::EventLoop    loop;
  xpp::WaitScope    scope(loop);
  xpp::sync::Notify n;

  n.notify_one(); // no waiters — no-op

  auto p = n.notified();
  n.notify_one();
  p.await();
}

// ── Multi-threaded tests ──────────────────────────────────────────

TEST(NotifyMtTest, WorkerNotifiesLoop) {
  xpp::sync::Notify n;
  xpp::EventLoop    loop;
  xpp::WaitScope    scope(loop);

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
  waiter().await();
  EXPECT_EQ(counter, 1);
  worker.join();
}

TEST(NotifyMtTest, MultipleWorkersNotifyWaiters) {
  xpp::sync::Notify n;
  xpp::EventLoop    loop;
  xpp::WaitScope    scope(loop);

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

  w1().await();
  w2().await();
  EXPECT_EQ(c1, 1);
  EXPECT_EQ(c2, 1);
  t1.join();
  t2.join();
}
