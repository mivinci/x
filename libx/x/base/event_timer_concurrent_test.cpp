/* event_timer_concurrent_test.cpp — Concurrent and hybrid tests */
#include "event_timer_test_helpers.h"

/* ───────────────────── Concurrent stress ───────────────────── */

TEST(BuiltinTimerConcurrent, CrossThreadStopStress) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  const int           N = 50, M = 4;
  std::atomic<int>    fired{0}, stopped{0};
  std::vector<xTimer> timers(N, nullptr);

  for (int i = 0; i < N; i++)
    timers[i] = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                            &fired, 2000, 0);

  std::thread threads[M];
  for (int t = 0; t < M; t++)
    threads[t] = std::thread([&timers, &stopped]() {
      for (int i = 0; i < N; i++)
        if (xTimerStop(timers[i]) == xErrno_Ok) stopped.fetch_add(1);
    });
  for (auto &th : threads)
    th.join();

  EXPECT_GE(stopped.load(), N) << "each timer stopped at least once";
  EXPECT_LE(stopped.load(), N * M) << "at most one per thread per timer";

  xEventLoopRun(loop, X_RUN_ONCE);
  EXPECT_EQ(fired.load(), 0) << "no timer should fire after cross-thread stop";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerConcurrent, CreateFromMultiplePosts) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  const int        N = 100;
  std::atomic<int> fired{0};
  int              timers_created = 0;

  for (int i = 0; i < N; i++) {
    auto *ctx = new std::pair<std::atomic<int> *, int *>(&fired, &timers_created);
    xEventLoopPost(
      loop,
      [](void *arg) {
        auto *p = static_cast<std::pair<std::atomic<int> *, int *> *>(arg);
        p->second++;
        xTimerStart([](void *a) { static_cast<std::atomic<int> *>(a)->fetch_add(1); }, p->first, 0,
                    0);
        delete p;
      },
      ctx);
  }

  {
    xTimer t =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 500, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (t) xTimerStop(t);
  }
  xEventLoopRun(loop, X_RUN_NOWAIT);
  EXPECT_EQ(fired.load(), N) << "all timers from Post callbacks should fire";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerConcurrent, RepeatTimerUnderCrossThreadStop) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> count{0};
  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                         &count, 20, 20);

  {
    xTimer t =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (t) xTimerStop(t);
  }
  EXPECT_GE(count.load(), 1) << "repeat timer should have fired";

  std::thread stopper([t]() { xTimerStop(t); });
  stopper.join();
  {
    xTimer stop =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (stop) xTimerStop(stop);
  }
  int after = count.load();
  {
    xTimer stop =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (stop) xTimerStop(stop);
  }
  EXPECT_EQ(count.load(), after) << "should not fire after cross-thread stop";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── Hybrid / orchestration ───────────────────── */

TEST(BuiltinTimerHybrid, StartTimerFromCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> first{0}, second{0};

  auto *pair = new std::pair<std::atomic<int> *, std::atomic<int> *>(&first, &second);
  xTimerStart(
    [](void *arg) {
      auto *ctx = static_cast<std::pair<std::atomic<int> *, std::atomic<int> *> *>(arg);
      ctx->first->fetch_add(1);
      xTimerStart([](void *a) { static_cast<std::atomic<int> *>(a)->fetch_add(1); }, ctx->second,
                  10, 0);
      delete ctx;
    },
    pair, 50, 0);

  {
    xTimer t =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (t) xTimerStop(t);
  }
  EXPECT_EQ(first.load(), 1) << "first timer should fire once";
  EXPECT_EQ(second.load(), 1) << "nested timer should fire too";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerHybrid, CancelFromAnotherCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  struct Ctx {
    std::atomic<int> a_count, b_count;
    xTimer           b_timer;
  };
  Ctx ctx{};

  xTimerStart(
    [](void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      c->a_count.fetch_add(1);
      xTimerStop(c->b_timer);
    },
    &ctx, 30, 0);

  ctx.b_timer =
    xTimerStart([](void *arg) { static_cast<Ctx *>(arg)->b_count.fetch_add(1); }, &ctx, 100, 0);
  ASSERT_NE(ctx.b_timer, nullptr);

  {
    xTimer t =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (t) xTimerStop(t);
  }
  EXPECT_EQ(ctx.a_count.load(), 1) << "timer A should fire";
  EXPECT_EQ(ctx.b_count.load(), 0) << "timer B should be cancelled by A";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerHybrid, InterleavedRepeatTimers) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fast{0}, slow{0};
  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fast, 20, 20);
  xTimer slow_t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                              &slow, 30, 60);

  {
    xTimer t =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (t) xTimerStop(t);
  }
  xTimerStop(slow_t);

  EXPECT_GE(fast.load(), 5) << "fast repeat should fire many times";
  EXPECT_LE(fast.load(), 16) << "fast should fire ~10 times";
  EXPECT_GE(slow.load(), 2) << "slow repeat should fire at least twice";
  EXPECT_LE(slow.load(), 6) << "slow should fire ~3 times";

  int after = slow.load();
  {
    xTimer t =
      xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (t) xTimerStop(t);
  }
  EXPECT_EQ(slow.load(), after) << "slow should not fire after stop";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}
