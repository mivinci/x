/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter_work_test.cpp - Tests for WorkAdapter / Promise::work()
 */
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

using namespace xpp;

/* ───────────────────── work() tests ───────────────────── */

TEST(PromiseWorkTest, BasicWork) {
  EventLoop loop;
  WaitScope scope(loop);
  int       result = work([] { return 42; }).await();
  EXPECT_EQ(result, 42);
}

TEST(PromiseWorkTest, WorkWithString) {
  EventLoop   loop;
  WaitScope   scope(loop);
  std::string result = work([] { return std::string("from thread pool"); }).await();
  EXPECT_EQ(result, "from thread pool");
}

TEST(PromiseWorkTest, WorkWithThenChain) {
  EventLoop loop;
  WaitScope scope(loop);
  int       result = work([] { return 10; }).then([](int x) { return x * 3; }).await();
  EXPECT_EQ(result, 30);
}

TEST(PromiseWorkTest, WorkWithSleep) {
  EventLoop loop;
  WaitScope scope(loop);
  auto      start  = std::chrono::steady_clock::now();
  int       result = work([] {
                 std::this_thread::sleep_for(std::chrono::milliseconds(50));
                 return 99;
               }).await();
  auto      ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
      .count();
  EXPECT_EQ(result, 99);
  EXPECT_GE(ms, 40);
}

TEST(PromiseWorkTest, WorkRaceWithTimer) {
  EventLoop loop;
  WaitScope scope(loop);
  int       result = race(work([] {
                      std::this_thread::sleep_for(std::chrono::milliseconds(100));
                      return 200;
                    }),
                          after(10).then([] { return -1; }))
                 .await();
  EXPECT_EQ(result, -1);
}
