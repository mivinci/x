/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter_timer_test.cpp - Tests for TimerAdapter / Promise::after()
 */
#include <chrono>

#include <gtest/gtest.h>
#include <xpp/promise.h>

using namespace xpp;

/* ───────────────────── after() tests ───────────────────── */

TEST(TimerAdapterTest, AfterBasic) {
  EventLoop loop;
  WaitScope scope(loop);
  auto      start = std::chrono::steady_clock::now();
  after(50).wait();
  auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
      .count();
  EXPECT_GE(ms, 40);
}

TEST(TimerAdapterTest, AfterZeroDelay) {
  EventLoop loop;
  WaitScope scope(loop);
  after(0).wait();
  SUCCEED();
}

TEST(TimerAdapterTest, AfterThenChain) {
  EventLoop loop;
  WaitScope scope(loop);
  int       val = 0;
  after(10).then([&val]() { val = 42; }).wait();
  EXPECT_EQ(val, 42);
}

TEST(TimerAdapterTest, AfterCancelOnEarlyDestruction) {
  EventLoop loop;
  WaitScope scope(loop);
  {
    auto p = after(10000);
    (void)p;
  }
  after(10).wait();
  SUCCEED();
}

TEST(TimerAdapterTest, AfterNestedAfter) {
  EventLoop loop;
  WaitScope scope(loop);
  auto      t0 = std::chrono::steady_clock::now();
  after(10).then([]() { return after(20); }).wait();
  auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
      .count();
  EXPECT_GE(ms, 25);
}

TEST(TimerAdapterTest, AfterSequentialWaits) {
  EventLoop loop;
  WaitScope scope(loop);
  after(10).wait();
  after(10).wait();
  SUCCEED();
}

TEST(TimerAdapterTest, AfterIndependentTimers) {
  EventLoop loop;
  WaitScope scope(loop);
  int       a = 0, b = 0;
  auto      pa = after(10).then([&]() { a = 42; });
  auto      pb = after(20).then([&]() { b = 99; });
  pa.wait();
  pb.wait();
  EXPECT_EQ(a, 42);
  EXPECT_EQ(b, 99);
}
