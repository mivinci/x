/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter_test.cpp - Tests for core adapter: PromiseResolver,
 * async(), Promise::adapt()
 */
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/promise_test_helper.h>
#include <xpp/timer.h>

#include <x/base/event.h>

using namespace xpp;

/* ───────────────────── PromiseResolver tests ───────────────────── */

TEST(PromiseResolverTest, ResolveBeforeWait) {
  EventLoop loop;
  WaitScope scope(loop);
  auto [p, r] = async<int>();
  r.resolve(42);
  EXPECT_EQ(p.wait(), 42);
}

TEST(PromiseResolverTest, ResolveDeferred) {
  EventLoop loop;
  WaitScope scope(loop);
  auto      pr = async<int>();
  auto      r  = std::move(pr.second);
  auto t = schedule_resolve(r, 99, 10);
  EXPECT_EQ(pr.first.wait(), 99);
}

TEST(PromiseResolverTest, ResolveVoid) {
  EventLoop loop;
  WaitScope scope(loop);
  auto      pr = async<void>();
  auto      r  = std::move(pr.second);
  auto t = schedule_resolve(r, 10);
  pr.first.wait();
  SUCCEED();
}

TEST(PromiseResolverTest, ResolveAfterPromiseDestroyed) {
  EventLoop            loop;
  WaitScope            scope(loop);
  PromiseResolver<int> r;
  {
    auto [p, r2] = async<int>();
    r            = std::move(r2);
  }
  r.resolve(42);
  SUCCEED();
}

TEST(PromiseResolverTest, DoubleResolve) {
  EventLoop loop;
  WaitScope scope(loop);
  auto [p, r] = async<int>();
  r.resolve(42);
  r.resolve(99);
  EXPECT_EQ(p.wait(), 42);
}

TEST(PromiseResolverTest, CrossThreadResolve) {
  EventLoop   loop;
  WaitScope   scope(loop);
  auto        pr = async<std::string>();
  auto        r  = std::move(pr.second);
  std::thread worker([&r]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.resolve(std::string("from another thread"));
  });
  EXPECT_EQ(pr.first.wait(), "from another thread");
  worker.join();
}

TEST(PromiseResolverTest, IsPending) {
  EventLoop loop;
  WaitScope scope(loop);
  auto [p, r] = async<int>();
  EXPECT_TRUE(r.is_pending());
  r.resolve(42);
  EXPECT_FALSE(r.is_pending());
  p.wait();
}

/* ───────────────────── Promise::adapt test ───────────────────── */

class CountingAdapter {
public:
  static int construct_count;
  static int destruct_count;
  CountingAdapter(PromiseResolver<int> &&r, int value)
      : m_resolver(std::move(r)), m_value(value),
        m_timer(0, 0, [this] { m_resolver.resolve(m_value); }) {
    construct_count++;
  }
  ~CountingAdapter() {
    destruct_count++;
  }
  CountingAdapter(const CountingAdapter &)            = delete;
  CountingAdapter &operator=(const CountingAdapter &) = delete;

private:
  PromiseResolver<int> m_resolver;
  int                  m_value;
  Timer                m_timer;
};

int CountingAdapter::construct_count = 0;
int CountingAdapter::destruct_count  = 0;

TEST(PromiseAdaptTest, CustomAdapter) {
  CountingAdapter::construct_count = 0;
  CountingAdapter::destruct_count  = 0;
  {
    EventLoop loop;
    WaitScope scope(loop);
    auto      p = Promise<int>::adapt<CountingAdapter>(42);
    EXPECT_EQ(CountingAdapter::construct_count, 1);
    EXPECT_EQ(p.wait(), 42);
  }
  EXPECT_EQ(CountingAdapter::destruct_count, 1);
}
