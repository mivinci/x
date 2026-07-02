/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer_test.cpp - Tests for xpp::Timer
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <xpp/timer.h>

#include <x/base/event.h>

/* ───────────────────── helpers ───────────────────── */

static void run_briefly(xEventLoop loop, uint64_t ms) {
  xEventLoopEnter(loop);
  /* Schedule a stop after `ms` to bound the run. */
  xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, NULL, ms, 0);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  xEventLoopLeave();
}

/* ───────────────────── repeating ───────────────────── */

TEST(TimerTest, RepeatingFiresMultipleTimes) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  xpp::Timer       t(20, [&]() { count.fetch_add(1); });

  run_briefly(loop.handle(), 100);

  EXPECT_GE(count.load(), 3);
}

/* ───────────────────── one-shot ───────────────────── */

TEST(TimerTest, OneShotFiresOnce) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  xpp::Timer       t(20, 0, [&]() { count.fetch_add(1); });

  run_briefly(loop.handle(), 100);

  EXPECT_EQ(count.load(), 1);
  EXPECT_FALSE(t.is_active());
}

/* ───────────────────── asymmetric repeating ───────────────────── */

TEST(TimerTest, AsymmetricRepeating) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  auto             t0 = std::chrono::steady_clock::now();
  xpp::Timer       t(10, 50, [&]() { count.fetch_add(1); });

  run_briefly(loop.handle(), 120);

  auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
      .count();

  EXPECT_GE(count.load(), 2);
  EXPECT_LE(ms, 500);
}

/* ───────────────────── stop ───────────────────── */

TEST(TimerTest, StopCancelsPending) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  xpp::Timer       t(10000, [&]() { count.fetch_add(1); });

  t.stop();
  EXPECT_FALSE(t.is_active());

  run_briefly(loop.handle(), 50);

  EXPECT_EQ(count.load(), 0);
}

TEST(TimerTest, StopIsIdempotent) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::Timer t(10000, []() {});
  t.stop();
  t.stop(); // no crash
  EXPECT_FALSE(t.is_active());
}

/* ───────────────────── start / resume ───────────────────── */

TEST(TimerTest, StartResumesAfterStop) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  xpp::Timer       t(20, [&]() { count.fetch_add(1); });

  t.stop();
  EXPECT_FALSE(t.start() == false || count.load() > 0); // start returns true
  EXPECT_TRUE(t.is_active());

  run_briefly(loop.handle(), 100);

  EXPECT_GE(count.load(), 1);
}

TEST(TimerTest, StartWhenAlreadyActiveReturnsFalse) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::Timer t(10000, []() {});
  EXPECT_FALSE(t.start()); // already active
}

/* ───────────────────── self-stop from callback ───────────────────── */

TEST(TimerTest, SelfStopFromCallback) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  xpp::Timer      *t_ptr = nullptr;
  xpp::Timer       t(20, [&]() {
    if (count.fetch_add(1) >= 2) {
      t_ptr->stop();
    }
  });
  t_ptr = &t;

  run_briefly(loop.handle(), 200);

  EXPECT_EQ(count.load(), 3); // stops on the 3rd callback
  EXPECT_FALSE(t.is_active());
}

/* ───────────────────── move ───────────────────── */

TEST(TimerTest, MoveTransfersOwnership) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  xpp::Timer       t1(20, [&]() { count.fetch_add(1); });
  EXPECT_TRUE(t1.is_active());

  xpp::Timer t2 = std::move(t1);
  EXPECT_FALSE(t1.is_active());
  EXPECT_TRUE(t2.is_active());

  run_briefly(loop.handle(), 100);

  EXPECT_GE(count.load(), 1);
}

TEST(TimerTest, MoveAssignStopsPrevious) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count1{0}, count2{0};
  xpp::Timer       t1(20, [&]() { count1.fetch_add(1); });
  xpp::Timer       t2(20, [&]() { count2.fetch_add(1); });

  t2 = std::move(t1);
  EXPECT_FALSE(t1.is_active());
  EXPECT_TRUE(t2.is_active());

  run_briefly(loop.handle(), 100);

  // t2 now has t1's callback (count1), not count2.
  EXPECT_GE(count1.load(), 1);
  // count2 should be 0 — t2's original timer was stopped by move-assign.
  EXPECT_EQ(count2.load(), 0);
}

/* ───────────────────── destructor ───────────────────── */

TEST(TimerTest, DestructorStopsActive) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::atomic<int> count{0};
  {
    xpp::Timer t(20, [&]() { count.fetch_add(1); });
    run_briefly(loop.handle(), 30); // let it fire once
  } // ~Timer here
  auto after_destruct = count.load();

  run_briefly(loop.handle(), 50);

  EXPECT_EQ(count.load(), after_destruct); // no more fires after destruct
}

/* ───────────────────── loop destroyed before fire ───────────────────── */

TEST(TimerTest, LoopDestroyedBeforeFire) {
  std::atomic<int> count{0};
  {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);
    xpp::Timer     t(60000, [&]() { count.fetch_add(1); });
    // ~Timer not called here — we want on_cancel_cb to fire
    // ~WaitScope leaves; ~EventLoop destroys loop; on_cancel_cb fires
  }
  EXPECT_EQ(count.load(), 0); // callback never ran
}

/* ───────────────────── handle / observers ───────────────────── */

TEST(TimerTest, HandleReturnsValidXTimer) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::Timer t(10000, []() {});
  EXPECT_NE(t.handle(), nullptr);
  EXPECT_TRUE(t.is_active());
  EXPECT_TRUE(static_cast<bool>(t));

  t.stop();
  EXPECT_EQ(t.handle(), nullptr);
  EXPECT_FALSE(t.is_active());
  EXPECT_FALSE(static_cast<bool>(t));
}

/* ───────────────────── construct outside WaitScope ───────────────────── */

TEST(TimerTest, ConstructOutsideWaitScope) {
  /* No WaitScope on this thread — xEventLoopCurrent() returns null. */
  xpp::Timer t(100, []() {});
  EXPECT_FALSE(t.is_active());
  EXPECT_EQ(t.handle(), nullptr);
}
