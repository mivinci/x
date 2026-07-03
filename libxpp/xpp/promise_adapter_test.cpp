/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter_test.cpp - Tests for PromiseResolver, AdaptedPromiseNode, TimerAdapter
 */
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <xpp/promise_adapter.h>

#include <x/base/event.h>

using namespace xpp;

/* ───────────────────── Helpers ───────────────────── */

static xTimer schedule_resolve_int(PromiseResolver<int> &r, int value,
                                    uint64_t delay_ms) {
  struct Ctx { PromiseResolver<int> *r; int value; };
  auto *ctx = new Ctx{&r, value};
  return xTimerStart([](void *a) { auto *c = static_cast<Ctx *>(a);
    c->r->resolve(c->value); delete c; }, ctx, nullptr, delay_ms, 0);
}

static xTimer schedule_resolve_void(PromiseResolver<void> &r, uint64_t delay_ms) {
  struct Ctx { PromiseResolver<void> *r; };
  auto *ctx = new Ctx{&r};
  return xTimerStart([](void *a) { auto *c = static_cast<Ctx *>(a);
    c->r->resolve(); delete c; }, ctx, nullptr, delay_ms, 0);
}

/* ───────────────────── PromiseResolver tests ───────────────────── */

TEST(PromiseResolverTest, ResolveBeforeWait) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  r.resolve(42);
  EXPECT_EQ(p.wait(), 42);
}

TEST(PromiseResolverTest, ResolveDeferred) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  schedule_resolve_int(r, 99, 10);
  EXPECT_EQ(p.wait(), 99);
}

TEST(PromiseResolverTest, ResolveVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<void>();
  schedule_resolve_void(r, 10);
  p.wait();
  SUCCEED();
}

TEST(PromiseResolverTest, ResolveAfterPromiseDestroyed) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  {
    // Destroy the promise
    auto p2 = std::move(p);
    (void)p2;
  }
  // p is now empty, p2 was destroyed at scope end... actually p2 is still alive
  // Let's do it properly
  // Actually, p was moved to p2 which is in the inner scope
  // When p2 goes out of scope, the Promise (and ManualResolveNode) is destroyed
  // Now r.resolve() should silently drop
  r.resolve(42);
  // No crash = pass
  SUCCEED();
}

TEST(PromiseResolverTest, ResolveAfterPromiseDestroyedProper) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  PromiseResolver<int> r;
  {
    auto [p, r2] = xpp::async<int>();
    r = std::move(r2);
    // p is destroyed when it goes out of scope
  }
  // Now the Promise is destroyed. r.resolve() should silently drop.
  r.resolve(42);
  SUCCEED();
}

TEST(PromiseResolverTest, DoubleResolve) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  r.resolve(42);
  r.resolve(99);  // Should be silently dropped
  EXPECT_EQ(p.wait(), 42);
}

TEST(PromiseResolverTest, CrossThreadResolve) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<std::string>();

  std::thread worker([&r]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.resolve(std::string("from another thread"));
  });

  EXPECT_EQ(p.wait(), "from another thread");
  worker.join();
}

TEST(PromiseResolverTest, IsPending) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [p, r] = xpp::async<int>();
  EXPECT_TRUE(r.is_pending());
  r.resolve(42);
  EXPECT_FALSE(r.is_pending());
  p.wait();
}

/* ───────────────────── TimerAdapter tests ───────────────────── */

TEST(TimerAdapterTest, AfterBasic) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto start = std::chrono::steady_clock::now();
  xpp::Promise<void>::after(50).wait();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  EXPECT_GE(ms, 40);
}

TEST(TimerAdapterTest, AfterZeroDelay) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::Promise<void>::after(0).wait();
  SUCCEED();
}

TEST(TimerAdapterTest, AfterThenChain) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int val = 0;
  xpp::Promise<void>::after(10).then([&val]() { val = 42; }).wait();
  EXPECT_EQ(val, 42);
}

TEST(TimerAdapterTest, AfterCancelOnEarlyDestruction) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  {
    auto p = xpp::Promise<void>::after(10000);
    // p is destroyed here — TimerAdapter destructor should call xTimerStop
  }
  // Run a short timer to verify the loop isn't corrupted
  xpp::Promise<void>::after(10).wait();
  SUCCEED();
}

TEST(TimerAdapterTest, AfterNestedAfter) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto t0 = std::chrono::steady_clock::now();
  xpp::Promise<void>::after(10)
      .then([]() { return xpp::Promise<void>::after(20); })
      .wait();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  EXPECT_GE(ms, 25);
}

/* ───────────────────── newAdaptedPromise test ───────────────────── */

// Custom adapter for testing
class CountingAdapter {
public:
  static int construct_count;
  static int destruct_count;

  CountingAdapter(PromiseResolver<int> &&r, int value)
      : m_resolver(std::move(r)), m_value(value) {
    construct_count++;
    // Schedule resolve on next event loop iteration
    xTimerStart([](void *a) {
      auto *self = static_cast<CountingAdapter *>(a);
      self->m_resolver.resolve(self->m_value);
    }, this, nullptr, 0, 0);
  }

  ~CountingAdapter() { destruct_count++; }

  CountingAdapter(const CountingAdapter &) = delete;
  CountingAdapter &operator=(const CountingAdapter &) = delete;

private:
  PromiseResolver<int> m_resolver;
  int m_value;
};

int CountingAdapter::construct_count = 0;
int CountingAdapter::destruct_count = 0;

TEST(NewAdaptedPromiseTest, CustomAdapter) {
  CountingAdapter::construct_count = 0;
  CountingAdapter::destruct_count = 0;

  {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);

    auto p = xpp::newAdaptedPromise<int, CountingAdapter>(42);
    EXPECT_EQ(CountingAdapter::construct_count, 1);
    EXPECT_EQ(p.wait(), 42);
  }

  EXPECT_EQ(CountingAdapter::destruct_count, 1);
}
