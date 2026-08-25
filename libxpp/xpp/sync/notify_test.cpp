/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * notify_test.cpp — Tests for xpp::sync::Notify.
 */
#include <chrono>
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

// ── N-6: lost-wakeup stress regression ──────────────────────────────
//
// Races a worker thread's notify_one() against the loop thread's
// notified() registration, fresh Notify per round. The old code
// incremented the pending count *outside* the waiter mutex, so a
// concurrently-registering waiter could re-check pending (relaxed, no
// ordering), see 0, park — and the notification sat unconsumed forever.
// (Sibling of issues/mpsc-single-slot-waiter-race.md.)

TEST(NotifyMtTest, StressPendingVsRegister) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  /* Probabilistic regression net, NOT a proof: the pre-fix bug window
   * (notifier's unlock → its pending increment vs the waiter's in-lock
   * re-check) is nanoseconds wide, so most rounds take the trivial
   * fast path. This exact shape has reproduced the hang (old code:
   * timeout kill) and is kept verbatim — adding a start barrier to
   * "align" the threads systematically AVOIDS the window (measured)
   * and loses coverage. The actual correctness guarantee is the
   * mutex fusion argument in notify.h, not this test. */
  for (int r = 0; r < 5000; ++r) {
    xpp::sync::Notify n;
    std::thread       worker([&n] { n.notify_one(); });
    n.notified().await(); // lost wakeup → permanent hang (timeout kills)
    worker.join();
  }
}
