/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * result_test.cpp - Tests for Result<T, E> and XPP_TRY macro.
 *
 * Covers XPP_TRY propagation, chaining, move semantics,
 * and edge cases.
 */

#include <string>
#include <utility>

#include <gtest/gtest.h>
#include <xpp/option.h>
#include <xpp/result.h>

namespace {

/* ── Helper types ───────────────────────────────────────────────── */

enum class TestError { A, B, C };

// Function helpers that exercise XPP_TRY from a real return context.

xpp::Result<int, TestError> try_ok(int val) {
  return xpp::ok(val);
}

xpp::Result<int, TestError> try_err(TestError e) {
  return xpp::err(e);
}

xpp::Result<std::string, TestError> try_string_ok(const char* s) {
  return xpp::ok(std::string(s));
}

/* ── XPP_TRY: basic ────────────────────────────────────────────── */

xpp::Result<int, TestError> single_try_ok() {
  int v = XPP_TRY(try_ok(42));
  return xpp::ok(v + 1);  // 43
}

TEST(ResultTry, OkUnwrapsValue) {
  auto r = single_try_ok();
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), 43);
}

xpp::Result<int, TestError> single_try_err() {
  int v = XPP_TRY(try_err(TestError::A));
  return xpp::ok(v);  // unreachable
}

TEST(ResultTry, ErrPropagatesImmediately) {
  auto r = single_try_err();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), TestError::A);
}

/* ── XPP_TRY: chaining ────────────────────────────────────────── */

xpp::Result<int, TestError> chain_all_ok(int a, int b, int c) {
  int x = XPP_TRY(try_ok(a));
  int y = XPP_TRY(try_ok(b));
  int z = XPP_TRY(try_ok(c));
  return xpp::ok(x + y + z);
}

TEST(ResultTry, ChainAllOk) {
  auto r = chain_all_ok(1, 2, 3);
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), 6);
}

xpp::Result<int, TestError> chain_mid_err() {
  int x = XPP_TRY(try_ok(1));
  int y = XPP_TRY(try_err(TestError::B));  // bails here
  int z = XPP_TRY(try_ok(3));              // unreached
  return xpp::ok(x + y + z);
}

TEST(ResultTry, ChainMidErrPropagates) {
  auto r = chain_mid_err();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), TestError::B);
}

xpp::Result<int, TestError> chain_first_err() {
  int x = XPP_TRY(try_err(TestError::C));  // bails immediately
  return xpp::ok(x);                        // unreached
}

TEST(ResultTry, ChainFirstErrPropagates) {
  auto r = chain_first_err();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), TestError::C);
}

/* ── XPP_TRY: move semantics ──────────────────────────────────── */

xpp::Result<std::string, TestError> try_move_ok() {
  std::string s = XPP_TRY(try_string_ok("hello"));
  s += " world";
  return xpp::ok(std::move(s));
}

TEST(ResultTry, MoveSemantics) {
  auto r = try_move_ok();
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), "hello world");
}

xpp::Result<std::string, TestError> try_move_err() {
  // Err path propagates moving the error.
  // try_err returns Result<int, TestError>, but the function returns
  // Result<string, TestError> — XPP_TRY turns the Err into Err for
  // the enclosing function's error type, so this works.
  int dummy = XPP_TRY(try_err(TestError::A));
  (void)dummy;
  return xpp::ok(std::string("unreached"));
}

TEST(ResultTry, MoveSemanticsErr) {
  auto r = try_move_err();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), TestError::A);
}

/* ── XPP_TRY: with direct xpp::ok / xpp::err carriers ────────── */

// xpp::ok(x) / xpp::err(e) return OkResult/ErtResult — carriers, not
// Result<T,E> themselves. XPP_TRY works on Result<T,E>, so assign to
// Result<T,E> first (the implicit conversion kicks in).

TEST(ResultTry, DirectOkInline) {
  auto fn = []() -> xpp::Result<int, TestError> {
    xpp::Result<int, TestError> r = xpp::ok(7);
    int v = XPP_TRY(r);
    EXPECT_EQ(v, 7);
    return xpp::ok(v * 2);
  };
  auto r = fn();
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), 14);
}

TEST(ResultTry, DirectErrInline) {
  auto fn = []() -> xpp::Result<int, TestError> {
    xpp::Result<int, TestError> r = xpp::err(TestError::B);
    int v = XPP_TRY(r);
    return xpp::ok(v);  // unreached
  };
  auto r = fn();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), TestError::B);
}

}  // namespace
