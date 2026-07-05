/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_coroutine_test.cpp — Tests for C++20 coroutine support.
 *
 * Requires C++20 coroutine support.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>
#include <xpp/promise_test_helper.h>

#include <x/base/event.h>

using namespace xpp;

/* ───────────────────── Coroutine functions ───────────────────── */

static Promise<int> simple_int() {
  co_return 42;
}

static Promise<std::string> simple_string() {
  co_return std::string("hello");
}

static Promise<void> simple_void() {
  co_return;
}

static Promise<int> await_resolve() {
  int x = co_await resolve(10);
  co_return x * 2;
}

static Promise<int> multiple_awaits() {
  int a = co_await resolve(1);
  int b = co_await resolve(2);
  int c = co_await resolve(3);
  co_return a + b + c;
}

static Promise<int> await_void() {
  co_await yield();
  co_return 42;
}

static Promise<int> await_after() {
  co_await after(10);
  co_return 99;
}

static Promise<int> await_work() {
  int x = co_await work([] { return 42; });
  co_return x + 1;
}

static Promise<int> await_async() {
  auto pr = async<int>();
  auto r  = std::move(pr.second);
  auto t  = schedule_resolve(r, 77, 10);
  int  x  = co_await std::move(pr.first);
  co_return x;
}

static Promise<int> inner_coro() {
  co_await after(10);
  co_return 100;
}

static Promise<int> outer_coro() {
  int x = co_await inner_coro();
  co_return x + 1;
}

static Promise<int> await_all() {
  auto t = co_await all(resolve(10), resolve(std::string("hi")));
  co_return std::get<0>(t) + static_cast<int>(std::get<1>(t).size());
}

static Promise<int> await_race() {
  int result = co_await race(resolve(42), after(100).then([] { return 0; }));
  co_return result;
}

static Promise<int> coro_for_then() {
  co_return 10;
}

static Promise<int> slow_coro() {
  co_await after(10000);
  co_return 42;
}

/* ── return_void() coverage ─────────────────────── */

/// Coroutine with no explicit co_return — compiler injects return_void().
static Promise<void> implicit_return_void() {
  // Fall off the end: return_void() called implicitly.
}

/// Void coroutine with explicit co_return (branch A).
static Promise<void> return_void_explicit() {
  co_return;
}

/// Void coroutine that co_awaits yield() then co_returns — return_void()
/// called after an intermediate suspension, verifying m_done/m_result
/// are set correctly regardless of prior await state.
static Promise<void> return_void_after_yield() {
  co_await yield();
  co_return;
}

/// Void coroutine used in a .then() chain — verifies return_void()
/// properly sets m_done so the downstream transform node can consume the Void.
static Promise<std::string> void_coro_then_chain() {
  co_await simple_void();
  co_return "after_void";
}

/* ───────────────────── Tests ───────────────────── */

TEST(PromiseCoroutineTest, SimpleReturn) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(simple_int().wait(), 42);
}

TEST(PromiseCoroutineTest, SimpleReturnString) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(simple_string().wait(), "hello");
}

TEST(PromiseCoroutineTest, ReturnVoid) {
  EventLoop loop;
  WaitScope scope(loop);
  simple_void().wait();
  SUCCEED();
}

/* ── return_void specific tests ────────────────── */

// TODO: fix return_void() in CoroutinePromise — coroutine with no co_return
// crashes. Tracked separately from arena work.
TEST(PromiseCoroutineTest, DISABLED_ImplicitReturnVoid) {
  // Coroutine with no co_return — falls off the end, compiler emits return_void().
  EventLoop loop;
  WaitScope scope(loop);
  implicit_return_void().wait();
  SUCCEED();
}

TEST(PromiseCoroutineTest, ReturnVoidAfterYield) {
  // return_void() called after co_await yield() — verifies m_done/m_result
  // are set correctly even when the coroutine has prior await state.
  EventLoop loop;
  WaitScope scope(loop);
  return_void_after_yield().wait();
  SUCCEED();
}

TEST(PromiseCoroutineTest, VoidCoroutineThenChain) {
  // Chain a .then() on a void coroutine — verifies return_void() sets m_done/m_result
  // so that TransformPromiseNode<void, void, Func>::poll() can consume the Void.
  EventLoop loop;
  WaitScope scope(loop);
  bool      flag = false;
  simple_void().then([&flag] { flag = true; }).wait();
  EXPECT_TRUE(flag);
}

TEST(PromiseCoroutineTest, VoidCoroutineCoAwaitInChain) {
  // Co_await a void coroutine from within another coroutine, then chain further.
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(void_coro_then_chain().wait(), "after_void");
}

TEST(PromiseCoroutineTest, AwaitResolve) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(await_resolve().wait(), 20);
}

TEST(PromiseCoroutineTest, MultipleAwaits) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(multiple_awaits().wait(), 6);
}

TEST(PromiseCoroutineTest, AwaitVoid) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(await_void().wait(), 42);
}

TEST(PromiseCoroutineTest, AwaitAfter) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(await_after().wait(), 99);
}

TEST(PromiseCoroutineTest, AwaitWork) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(await_work().wait(), 43);
}

TEST(PromiseCoroutineTest, AwaitAsync) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(await_async().wait(), 77);
}

TEST(PromiseCoroutineTest, NestedCoroutines) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(outer_coro().wait(), 101);
}

TEST(PromiseCoroutineTest, AwaitAll) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(await_all().wait(), 12);
}

TEST(PromiseCoroutineTest, AwaitRace) {
  EventLoop loop;
  WaitScope scope(loop);
  EXPECT_EQ(await_race().wait(), 42);
}

TEST(PromiseCoroutineTest, CoroutineThenChain) {
  EventLoop loop;
  WaitScope scope(loop);
  int       result = coro_for_then().then([](int x) { return x * 3; }).wait();
  EXPECT_EQ(result, 30);
}

TEST(PromiseCoroutineTest, EarlyDestruction) {
  EventLoop loop;
  WaitScope scope(loop);
  {
    auto p = slow_coro();
    (void)p;
  }
  SUCCEED();
}
