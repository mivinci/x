/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_combinators_test.cpp - Tests for xpp::all() and xpp::race()
 */
#include <chrono>
#include <string>

#include <gtest/gtest.h>
#include <xpp/promise_combinators.h>
#include <xpp/promise_test_helper.h>

#include <x/base/event.h>

using namespace xpp;

/* ───────────────────── all: immediate ───────────────────── */

TEST(AllTest, ImmediateHeterogeneous) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t = xpp::all(xpp::resolve(42), xpp::resolve(std::string("hi"))).wait();
  EXPECT_EQ(std::get<0>(t), 42);
  EXPECT_EQ(std::get<1>(t), "hi");
}

TEST(AllTest, ImmediateAllVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::all(xpp::yield(), xpp::yield(), xpp::yield()).wait();
  SUCCEED();
}

TEST(AllTest, ImmediateVoidAndValue) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t = xpp::all(xpp::yield(), xpp::resolve(7)).wait();
  EXPECT_EQ(std::get<1>(t), 7);
}

TEST(AllTest, ImmediateThreeTypes) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t = xpp::all(xpp::resolve(1), xpp::resolve(2.5), xpp::resolve(std::string("three"))).wait();
  EXPECT_EQ(std::get<0>(t), 1);
  EXPECT_DOUBLE_EQ(std::get<1>(t), 2.5);
  EXPECT_EQ(std::get<2>(t), "three");
}

/* ───────────────────── all: deferred ───────────────────── */

TEST(AllTest, DeferredHeterogeneous) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p1, r1] = xpp::async<int>();
  auto [p2, r2] = xpp::async<std::string>();

  auto t1 = schedule_resolve(r1, 99, 10);
  auto t2 = schedule_resolve(r2, std::string("deferred"), 30);

  auto t = xpp::all(std::move(p1), std::move(p2)).wait();
  EXPECT_EQ(std::get<0>(t), 99);
  EXPECT_EQ(std::get<1>(t), "deferred");
}

TEST(AllTest, DeferredAllVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p1, r1] = xpp::async<void>();
  auto [p2, r2] = xpp::async<void>();

  auto t1 = schedule_resolve(r1, 10);
  auto t2 = schedule_resolve(r2, 30);

  xpp::all(std::move(p1), std::move(p2)).wait();
  SUCCEED();
}

TEST(AllTest, DeferredWithTimers) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto start = std::chrono::steady_clock::now();
  xpp::all(xpp::after(10), xpp::after(50)).wait();
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 40);
}

/* ───────────────────── all: chaining ───────────────────── */

TEST(AllTest, ThenChain) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int sum = xpp::all(xpp::resolve(10), xpp::resolve(20))
              .then([](std::tuple<int, int> t) { return std::get<0>(t) + std::get<1>(t); })
              .wait();
  EXPECT_EQ(sum, 30);
}

/* ───────────────────── race: immediate ───────────────────── */

TEST(RaceTest, ImmediateFirstWins) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::race(xpp::resolve(1), xpp::resolve(2)).wait();
  EXPECT_EQ(result, 1);
}

TEST(RaceTest, ImmediateOneDeferred) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::race(xpp::resolve(42), xpp::after(100).then([] { return 99; })).wait();
  EXPECT_EQ(result, 42);
}

/* ───────────────────── race: deferred ───────────────────── */

TEST(RaceTest, FasterTimerWins) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result =
    xpp::race(xpp::after(50).then([] { return 1; }), xpp::after(10).then([] { return 2; })).wait();
  EXPECT_EQ(result, 2);
}

TEST(RaceTest, DeferredViaResolver) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p1, r1] = xpp::async<int>();
  auto [p2, r2] = xpp::async<int>();

  auto t2 = schedule_resolve(r2, 77, 10); // r2 resolves first
  auto t1 = schedule_resolve(r1, 88, 50);

  int result = xpp::race(std::move(p1), std::move(p2)).wait();
  EXPECT_EQ(result, 77);
}

/* ───────────────────── race: void ───────────────────── */

TEST(RaceTest, VoidRace) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::race(xpp::after(10), xpp::after(50)).wait();
  SUCCEED();
}

TEST(RaceTest, VoidRaceImmediate) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::race(xpp::yield(), xpp::after(50)).wait();
  SUCCEED();
}

/* ───────────────────── race: timeout pattern ───────────────────── */

TEST(RaceTest, TimeoutPattern) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  // Simulate: fetch takes 100ms, timeout is 10ms → timeout wins
  int result = xpp::race(xpp::after(100).then([] { return 200; }), // "fetch"
                         xpp::after(10).then([] { return -1; })    // timeout
                         )
                 .wait();
  EXPECT_EQ(result, -1);
}

/* ───────────────────── race: destruction safety ───────────────────── */

TEST(RaceTest, TimerStoppedOnEarlyResolve) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  // When the immediate promise wins, the TimerPromiseNode must be
  // destroyed and its timer stopped. If the timer fires after the
  // node is destroyed, it would be a use-after-free.
  xpp::race(xpp::resolve(42), xpp::after(10000).then([] { return 0; })).wait();

  // Run the loop briefly to ensure no crash from a stale timer
  xpp::after(50).wait();
  SUCCEED();
}

/* ───────────────────── race: chaining ───────────────────── */

TEST(RaceTest, ThenChain) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int doubled = xpp::race(xpp::resolve(21), xpp::after(50).then([] { return 99; }))
                  .then([](int x) { return x * 2; })
                  .wait();
  EXPECT_EQ(doubled, 42);
}
