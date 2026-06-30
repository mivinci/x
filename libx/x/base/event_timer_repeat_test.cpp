#include <x/base/event.h>

#include <gtest/gtest.h>

#include <x/base/test_helper.h>

/* ───────────────────── Repeat timer ───────────────────── */

TEST(BuiltinTimerRepeat, FiresMultipleTimes) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> count{0};
  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &count, 10, 10);

  run_for(loop, 2000);
  xEventLoopRun(loop, X_RUN_NOWAIT);

  EXPECT_GE(count.load(), 3) << "repeat timer should fire multiple times";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerRepeat, StopRepeatTimer) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> count{0};
  xTimer t = xTimerStart(
    [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &count, 10, 10);
  ASSERT_NE(t, nullptr);

  run_until_count(loop, count, 3, 10000);
  EXPECT_GE(count.load(), 1) << "should have fired at least once before stop";

  int before = count.load();
  EXPECT_EQ(xTimerStop(t), xErrno_Ok);
  xEventLoopRun(loop, X_RUN_ONCE);
  EXPECT_EQ(count.load(), before) << "should not fire after stop";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── Edge cases ───────────────────── */

TEST(BuiltinTimerEdge, SelfStopInCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  struct SelfCtx { std::atomic<int> count; xTimer timer; };
  SelfCtx ctx{};
  ctx.timer = xTimerStart(
    [](void *arg) {
      auto *sc = static_cast<SelfCtx *>(arg);
      sc->count.fetch_add(1);
      if (sc->count.load() >= 2) xTimerStop(sc->timer);
    }, &ctx, 10, 10);
  ASSERT_NE(ctx.timer, nullptr);

  run_for(loop, 500);
  EXPECT_LE(ctx.count.load(), 3) << "self-stop prevented further fires";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerEdge, BulkStress) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  const int N = 200;
  std::atomic<int> counter{0};
  for (int i = 0; i < N; i++)
    xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &counter, static_cast<uint64_t>(i), 0);

  run_until_count(loop, counter, N, 10000);
  xEventLoopRun(loop, X_RUN_NOWAIT);
  EXPECT_EQ(counter.load(), N) << "all timers should fire";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerEdge, MixOneShotAndRepeat) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> oneshot{0}, repeat{0};
  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &oneshot, 10, 0);
  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &repeat, 10, 10);

  {
    xTimer stop = xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 100, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (stop) xTimerStop(stop);
  }
  xTimerStop(t);

  EXPECT_EQ(oneshot.load(), 1) << "one-shot should fire exactly once";
  EXPECT_GE(repeat.load(), 3) << "repeat should fire multiple times";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

