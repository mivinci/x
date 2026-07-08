/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_utils_test.cpp - Tests for xpp::try_next()
 */
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <xpp/promise_test_helper.h>
#include <xpp/promise_utils.h>

#include <x/base/event.h>

using namespace xpp;

/* ───────────────────── try_next: immediate ───────────────────── */

TEST(TryNextTest, FirstSucceedsImmediate) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {10, 20, 30};
  int              count = 0;

  auto r      = xpp::try_next(std::move(items), [&](int x) -> Promise<Result<int, int>> {
    count++;
    return xpp::resolve(Result<int, int>(ok, x * 2));
  })();
  int  result = r.await().unwrap();
  EXPECT_EQ(result, 20);
  EXPECT_EQ(count, 1);
}

TEST(TryNextTest, SecondSucceeds) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {10, 20, 30};
  int              count = 0;

  auto r      = xpp::try_next(std::move(items), [&](int x) -> Promise<Result<int, int>> {
    count++;
    if (x == 10) {
      return xpp::resolve(Result<int, int>(err, -x));
    }
    return xpp::resolve(Result<int, int>(ok, x));
  })();
  int  result = r.await().unwrap();
  EXPECT_EQ(result, 20);
  EXPECT_EQ(count, 2);
}

TEST(TryNextTest, AllFailReturnsLastError) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {10, 20, 30};
  int              count = 0;

  auto r      = xpp::try_next(std::move(items), [&](int x) -> Promise<Result<int, int>> {
    count++;
    return xpp::resolve(Result<int, int>(err, -x));
  })();
  int  errval = r.await().unwrap_err();
  EXPECT_EQ(errval, -30); // last error
  EXPECT_EQ(count, 3);
}

TEST(TryNextTest, SingleItemSucceeds) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {99};

  auto r = xpp::try_next(std::move(items), [&](int x) -> Promise<Result<int, int>> {
    return xpp::resolve(Result<int, int>(ok, x));
  })();
  EXPECT_EQ(r.await().unwrap(), 99);
}

TEST(TryNextTest, SingleItemFails) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {7};

  auto r = xpp::try_next(std::move(items), [&](int x) -> Promise<Result<int, int>> {
    return xpp::resolve(Result<int, int>(err, x));
  })();
  EXPECT_EQ(r.await().unwrap_err(), 7);
}

/* ───────────────────── try_next: deferred ───────────────────── */

TEST(TryNextTest, DeferredFirstSucceeds) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {10, 20, 30};

  auto async_result = xpp::async<Result<int, int>>();
  auto t            = schedule_resolve(async_result.second, Result<int, int>(ok, 42), 10);

  auto result =
    xpp::try_next(std::move(items),
                  [p = std::move(async_result.first)](int) mutable -> Promise<Result<int, int>> {
                    return std::move(p);
                  })();
  EXPECT_EQ(result.await().unwrap(), 42);
}

TEST(TryNextTest, DeferredSecondSucceeds) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {10, 20, 30};
  int              count = 0;

  auto ar1 = xpp::async<Result<int, int>>();
  auto ar2 = xpp::async<Result<int, int>>();

  // r1 returns error at 10ms, r2 returns success at 20ms
  auto t1 = schedule_resolve(ar1.second, Result<int, int>(err, -1), 10);
  auto t2 = schedule_resolve(ar2.second, Result<int, int>(ok, 99), 20);

  auto result = xpp::try_next(std::move(items),
                              [&, p1 = std::move(ar1.first), p2 = std::move(ar2.first)](
                                int) mutable -> Promise<Result<int, int>> {
                                count++;
                                if (count == 1) return std::move(p1);
                                return std::move(p2);
                              })();
  EXPECT_EQ(result.await().unwrap(), 99);
  EXPECT_EQ(count, 2);
}

/* ───────────────────── try_next: string items ───────────────────── */

TEST(TryNextTest, StringItems) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<std::string> items = {"a", "b", "c"};

  auto r =
    xpp::try_next(std::move(items), [](const std::string &s) -> Promise<Result<std::string, int>> {
      if (s == "b") {
        return xpp::resolve(Result<std::string, int>(ok, s));
      }
      return xpp::resolve(Result<std::string, int>(err, 0));
    })();
  EXPECT_EQ(r.await().unwrap(), "b");
}

/* ───────────────────── try_next: multi-type Result ───────────────── */

TEST(TryNextTest, OkAndErrDifferentTypes) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::vector<int> items = {1, 2, 3};

  // Ok = std::string, Err = int
  auto r = xpp::try_next(std::move(items), [](int x) -> Promise<Result<std::string, int>> {
    if (x == 2) {
      return xpp::resolve(Result<std::string, int>(ok, "success"));
    }
    return xpp::resolve(Result<std::string, int>(err, x));
  })();
  EXPECT_EQ(r.await().unwrap(), "success");
}
