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

#include <x/base/event.h>

/* ───────────────────── Helpers ───────────────────── */

static xTimer schedule_resolve_int(xpp::PromiseResolver<int> &r, int value, uint64_t delay_ms) {
  struct Ctx {
    xpp::PromiseResolver<int> *r;
    int                        value;
  };
  auto *ctx = new Ctx{&r, value};
  return xTimerStart(
    [](void *a) {
      auto *c = static_cast<Ctx *>(a);
      c->r->resolve(c->value);
      delete c;
    },
    ctx, nullptr, delay_ms, 0);
}

static xTimer schedule_resolve_str(xpp::PromiseResolver<std::string> &r, const char *val,
                                   uint64_t delay_ms) {
  struct Ctx {
    xpp::PromiseResolver<std::string> *r;
    std::string                        val;
  };
  auto *ctx = new Ctx{&r, val};
  return xTimerStart(
    [](void *a) {
      auto *c = static_cast<Ctx *>(a);
      c->r->resolve(std::move(c->val));
      delete c;
    },
    ctx, nullptr, delay_ms, 0);
}

static xTimer schedule_resolve_void(xpp::PromiseResolver<void> &r, uint64_t delay_ms) {
  struct Ctx {
    xpp::PromiseResolver<void> *r;
  };
  auto *ctx = new Ctx{&r};
  return xTimerStart(
    [](void *a) {
      auto *c = static_cast<Ctx *>(a);
      c->r->resolve();
      delete c;
    },
    ctx, nullptr, delay_ms, 0);
}

/* ───────────────────── all: immediate ───────────────────── */

TEST(AllTest, ImmediateHeterogeneous) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t =
    xpp::all(xpp::Promise<int>::resolve(42), xpp::Promise<std::string>::resolve(std::string("hi")))
      .wait();
  EXPECT_EQ(std::get<0>(t), 42);
  EXPECT_EQ(std::get<1>(t), "hi");
}

TEST(AllTest, ImmediateAllVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::all(xpp::Promise<void>::resolve(), xpp::Promise<void>::resolve(),
           xpp::Promise<void>::resolve())
    .wait();
  SUCCEED();
}

TEST(AllTest, ImmediateVoidAndValue) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t = xpp::all(xpp::Promise<void>::resolve(), xpp::Promise<int>::resolve(7)).wait();
  EXPECT_EQ(std::get<1>(t), 7);
}

TEST(AllTest, ImmediateThreeTypes) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t = xpp::all(xpp::Promise<int>::resolve(1), xpp::Promise<double>::resolve(2.5),
                    xpp::Promise<std::string>::resolve(std::string("three")))
             .wait();
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

  schedule_resolve_int(r1, 99, 10);
  schedule_resolve_str(r2, "deferred", 30);

  auto t = xpp::all(std::move(p1), std::move(p2)).wait();
  EXPECT_EQ(std::get<0>(t), 99);
  EXPECT_EQ(std::get<1>(t), "deferred");
}

TEST(AllTest, DeferredAllVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p1, r1] = xpp::async<void>();
  auto [p2, r2] = xpp::async<void>();

  schedule_resolve_void(r1, 10);
  schedule_resolve_void(r2, 30);

  xpp::all(std::move(p1), std::move(p2)).wait();
  SUCCEED();
}

TEST(AllTest, DeferredWithTimers) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto start = std::chrono::steady_clock::now();
  xpp::all(xpp::Promise<void>::after(10), xpp::Promise<void>::after(50)).wait();
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 40);
}

/* ───────────────────── all: chaining ───────────────────── */

TEST(AllTest, ThenChain) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int sum = xpp::all(xpp::Promise<int>::resolve(10), xpp::Promise<int>::resolve(20))
              .then([](std::tuple<int, int> t) { return std::get<0>(t) + std::get<1>(t); })
              .wait();
  EXPECT_EQ(sum, 30);
}

/* ───────────────────── race: immediate ───────────────────── */

TEST(RaceTest, ImmediateFirstWins) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::race(xpp::Promise<int>::resolve(1), xpp::Promise<int>::resolve(2)).wait();
  EXPECT_EQ(result, 1);
}

TEST(RaceTest, ImmediateOneDeferred) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::race(xpp::Promise<int>::resolve(42),
                         xpp::Promise<void>::after(100).then([] { return 99; }))
                 .wait();
  EXPECT_EQ(result, 42);
}

/* ───────────────────── race: deferred ───────────────────── */

TEST(RaceTest, FasterTimerWins) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int result = xpp::race(xpp::Promise<void>::after(50).then([] { return 1; }),
                         xpp::Promise<void>::after(10).then([] { return 2; }))
                 .wait();
  EXPECT_EQ(result, 2);
}

TEST(RaceTest, DeferredViaResolver) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p1, r1] = xpp::async<int>();
  auto [p2, r2] = xpp::async<int>();

  schedule_resolve_int(r2, 77, 10); // r2 resolves first
  schedule_resolve_int(r1, 88, 50);

  int result = xpp::race(std::move(p1), std::move(p2)).wait();
  EXPECT_EQ(result, 77);
}

/* ───────────────────── race: void ───────────────────── */

TEST(RaceTest, VoidRace) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::race(xpp::Promise<void>::after(10), xpp::Promise<void>::after(50)).wait();
  SUCCEED();
}

TEST(RaceTest, VoidRaceImmediate) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::race(xpp::Promise<void>::resolve(), xpp::Promise<void>::after(50)).wait();
  SUCCEED();
}

/* ───────────────────── race: timeout pattern ───────────────────── */

TEST(RaceTest, TimeoutPattern) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  // Simulate: fetch takes 100ms, timeout is 10ms → timeout wins
  int result = xpp::race(xpp::Promise<void>::after(100).then([] { return 200; }), // "fetch"
                         xpp::Promise<void>::after(10).then([] { return -1; })    // timeout
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
  xpp::race(xpp::Promise<int>::resolve(42), xpp::Promise<void>::after(10000).then([] { return 0; }))
    .wait();

  // Run the loop briefly to ensure no crash from a stale timer
  xpp::Promise<void>::after(50).wait();
  SUCCEED();
}

/* ───────────────────── race: chaining ───────────────────── */

TEST(RaceTest, ThenChain) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int doubled =
    xpp::race(xpp::Promise<int>::resolve(21), xpp::Promise<void>::after(50).then([] { return 99; }))
      .then([](int x) { return x * 2; })
      .wait();
  EXPECT_EQ(doubled, 42);
}
