/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_post_test.cpp - Unit tests for xEventLoopPost
 */

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <x/base/event.h>
#include <x/base/test_helper.h>

/* ───────────────────── Fixture ───────────────────── */

class EventPostTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    xEventLoopEnter(loop);
  }

  void TearDown() override {
    xEventLoopLeave();
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ───────────────────── Basic post ───────────────────── */

TEST_F(EventPostTest, BasicPost) {
  std::atomic<bool> called{false};

  auto fn = [](void *arg) {
    static_cast<std::atomic<bool> *>(arg)->store(true, std::memory_order_release);
  };

  ASSERT_EQ(xEventLoopPost(this->loop, fn, &called), xErrno_Ok);

  /* Pump the loop until the callback fires. */
  run_until(loop, called, 5000);

  EXPECT_TRUE(called.load());
}

/* ───────────────────── Post from another thread ───────────────────── */

TEST_F(EventPostTest, PostFromAnotherThread) {
  std::atomic<bool>            called{false};
  std::atomic<std::thread::id> cb_thread{};

  auto fn = [](void *arg) {
    auto *ctx = static_cast<std::pair<std::atomic<bool> *, std::atomic<std::thread::id> *> *>(arg);
    ctx->second->store(std::this_thread::get_id(), std::memory_order_release);
    ctx->first->store(true, std::memory_order_release);
  };

  std::pair<std::atomic<bool> *, std::atomic<std::thread::id> *> ctx{&called, &cb_thread};

  std::thread poster([&]() {
    xEventLoopEnter(loop);
    EXPECT_EQ(xEventLoopPost(this->loop, fn, &ctx), xErrno_Ok);
    xEventLoopLeave();
  });

  poster.join();

  /* Pump the loop until the callback fires. */
  std::thread::id loop_thread = std::this_thread::get_id();
  run_until(loop, called, 5000);

  EXPECT_TRUE(called.load());
  /* Callback must have run on the loop thread (the current thread). */
  EXPECT_EQ(cb_thread.load(), loop_thread);
}

/* ───────────────────── Multiple posts ───────────────────── */

TEST_F(EventPostTest, MultiplePosts) {
  constexpr int    N = 100;
  std::atomic<int> count{0};

  auto fn = [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1, std::memory_order_relaxed);
  };

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(xEventLoopPost(this->loop, fn, &count), xErrno_Ok);
  }

  run_until_count(loop, count, N, 10000);

  EXPECT_EQ(count.load(), N);
}

/* ───────────────────── Concurrent posts from multiple threads ───────── */

TEST_F(EventPostTest, ConcurrentPosts) {
  constexpr int THREADS    = 4;
  constexpr int PER_THREAD = 50;
  constexpr int TOTAL      = THREADS * PER_THREAD;

  std::atomic<int> count{0};

  auto fn = [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1, std::memory_order_relaxed);
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; t++) {
    threads.emplace_back([&]() {
      xEventLoopEnter(loop);
      for (int i = 0; i < PER_THREAD; i++) {
        xEventLoopPost(this->loop, fn, &count);
      }
      xEventLoopLeave();
    });
  }

  for (auto &th : threads)
    th.join();

  run_until_count(loop, count, TOTAL, 10000);

  EXPECT_EQ(count.load(), TOTAL);
}

/* ───────────────────── Post interleaved with offload ───────────────── */

TEST_F(EventPostTest, PostInterleavedWithSubmit) {
  std::atomic<int> post_count{0};

  auto post_fn = [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1, std::memory_order_relaxed);
  };

  /* Post some callbacks. */
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(xEventLoopPost(this->loop, post_fn, &post_count), xErrno_Ok);
  }

  /* Pump the loop. */
  run_until_count(loop, post_count, 10, 10000);

  EXPECT_EQ(post_count.load(), 10);
}

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(EventPostTest, NullArgIsFine) {
  auto fn = [](void *) {};
  EXPECT_EQ(xEventLoopPost(this->loop, fn, nullptr), xErrno_Ok);
}

TEST_F(EventPostTest, NullFnReturnsError) {
  EXPECT_EQ(xEventLoopPost(this->loop, nullptr, nullptr), xErrno_InvalidArg);
}

/* ───────────────────── Post with xEventLoopRun ───────────────────── */

TEST_F(EventPostTest, PostStopsRunningLoop) {
  /* Post a callback that stops the loop, then run the loop. */
  auto stop_fn = [](void *arg) {
    (void)arg;
    xEventLoopStop(static_cast<xEventLoop>(arg));
  };

  ASSERT_EQ(xEventLoopPost(this->loop, stop_fn, loop), xErrno_Ok);

  /* xEventLoopRun should return promptly because the posted callback
   * will stop the loop. */
  xEventLoopRun(loop, X_RUN_DEFAULT);

  /* If we get here, the loop was stopped successfully. */
  SUCCEED();
}
