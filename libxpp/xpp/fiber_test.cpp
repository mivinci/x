/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber_test.cpp — Tests for xpp::xpp::fiber() stackful coroutine integration.
 */

#include <gtest/gtest.h>

#include <xpp/fiber.h>
#include <xpp/promise.h>

namespace {

/* ═══════════════════════════════════════════════════════════════════
 *  Basic fiber lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

TEST(FiberTest, ImmediateReturn) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() { return 42; });
  EXPECT_EQ(std::move(p).wait(), 42);
}

TEST(FiberTest, ImmediateReturnVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  bool called = false;
  auto p      = xpp::fiber([&called]() { called = true; });
  std::move(p).wait();
  EXPECT_TRUE(called);
}

TEST(FiberTest, DefaultStackSize) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() { return std::string("hello"); });
  EXPECT_EQ(std::move(p).wait(), "hello");
}

TEST(FiberTest, CustomStackSize) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber(32768, []() { return 99; });
  EXPECT_EQ(std::move(p).wait(), 99);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Fiber with Promise::wait()
 * ═══════════════════════════════════════════════════════════════════ */

TEST(FiberTest, WaitOnResolvedPromise) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() {
    auto r = xpp::resolve(7).wait();
    return r * 2;
  });
  EXPECT_EQ(std::move(p).wait(), 14);
}

TEST(FiberTest, WaitOnPendingPromise) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() {
    auto r = xpp::yield().then([]() { return 10; });
    return r.wait();
  });
  EXPECT_EQ(std::move(p).wait(), 10);
}

TEST(FiberTest, ChainWaitMultiple) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() {
    int a = xpp::resolve(1).wait();
    int b = xpp::resolve(2).wait();
    int c = xpp::resolve(3).wait();
    return a + b + c;
  });
  EXPECT_EQ(std::move(p).wait(), 6);
}

TEST(FiberTest, WaitInsideThen) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() {
    return xpp::yield().then([]() { return 5; }).then([](int x) { return x * 2; }).wait();
  });
  EXPECT_EQ(std::move(p).wait(), 10);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Multiple fibers
 * ═══════════════════════════════════════════════════════════════════ */

TEST(FiberTest, TwoFibersSequential) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p1 = xpp::fiber([]() { return xpp::resolve(1).wait(); });
  auto p2 = xpp::fiber([]() { return xpp::resolve(2).wait(); });

  EXPECT_EQ(std::move(p1).wait(), 1);
  EXPECT_EQ(std::move(p2).wait(), 2);
}

TEST(FiberTest, TenFibersInSequence) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int sum = 0;
  for (int i = 0; i < 10; i++) {
    auto p = xpp::fiber([i]() { return i * 10; });
    sum += std::move(p).wait();
  }
  EXPECT_EQ(sum, 450);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Fiber with complex types
 * ═══════════════════════════════════════════════════════════════════ */

TEST(FiberTest, StringReturn) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() {
    std::string a = xpp::resolve(std::string("hello")).wait();
    std::string b = xpp::resolve(std::string(" world")).wait();
    return a + b;
  });
  EXPECT_EQ(std::move(p).wait(), "hello world");
}

TEST(FiberTest, MoveOnlyType) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto p = xpp::fiber([]() {
    auto a = xpp::resolve(std::unique_ptr<int>(new int(42))).wait();
    return *a;
  });
  EXPECT_EQ(std::move(p).wait(), 42);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Lambda captures
 * ═══════════════════════════════════════════════════════════════════ */

TEST(FiberTest, CaptureByValue) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int  x = 100;
  auto p = xpp::fiber([x]() { return x + 1; });
  EXPECT_EQ(std::move(p).wait(), 101);
}

TEST(FiberTest, CaptureByReference) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int  value = 0;
  auto p     = xpp::fiber([&value]() { value = 42; });
  std::move(p).wait();
  EXPECT_EQ(value, 42);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Recursive fibers — spawn fiber inside fiber
 * ═══════════════════════════════════════════════════════════════════ */

TEST(FiberTest, NestedFiberChildReturnsToParent) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto outer = xpp::fiber(65536, []() {
    // Spawn inner fiber — it yields back to us, not to main
    auto inner = xpp::fiber(32768, []() {
      return 42;
    });
    // .wait() on inner: inner suspends, yields back to outer
    // When inner resolves, outer resumes and gets the value
    return std::move(inner).wait() + 1;
  });

  EXPECT_EQ(std::move(outer).wait(), 43);
}

TEST(FiberTest, NestedFiberPendingPromises) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto outer = xpp::fiber(65536, []() {
    auto inner = xpp::fiber(32768, []() {
      int a = xpp::resolve(3).wait();
      int b = xpp::resolve(4).wait();
      return a + b;
    });
    int x = std::move(inner).wait();
    int y = xpp::resolve(10).wait();
    return x + y;
  });

  EXPECT_EQ(std::move(outer).wait(), 17);
}

TEST(FiberTest, NestedFiberVoid) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  bool inner_called = false;
  bool outer_called = false;

  auto outer = xpp::fiber(65536, [&]() {
    auto inner = xpp::fiber(32768, [&]() {
      inner_called = true;
      // void fiber — no return value
    });
    std::move(inner).wait();
    outer_called = true;
  });

  std::move(outer).wait();
  EXPECT_TRUE(inner_called);
  EXPECT_TRUE(outer_called);
}

TEST(FiberTest, TripleNestedFiber) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto level1 = xpp::fiber(65536, []() {
    auto level2 = xpp::fiber(32768, []() {
      auto level3 = xpp::fiber(16384, []() {
        return xpp::resolve(100).wait();
      });
      return std::move(level3).wait() + 10;
    });
    return std::move(level2).wait() + 1;
  });

  EXPECT_EQ(std::move(level1).wait(), 111);  /* 100 + 10 + 1 */
}

} // namespace