/* event_timer_misc_test.cpp — Miscellaneous timer tests */
#include "event_timer_test_helpers.h"

/* ───────────────────── Multiple timers ordering ───────────────────── */
/* ───────────────────── Multiple timers ordering ───────────────────── */

TEST(BuiltinTimerOrder, MultipleTimersInOrder) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::vector<int> order;
  std::mutex       mu;

  struct Ctx {
    std::vector<int> *order;
    std::mutex       *mu;
    int               id;
  };

  auto fn = [](void *arg) {
    auto                       *ctx = static_cast<Ctx *>(arg);
    std::lock_guard<std::mutex> lock(*ctx->mu);
    ctx->order->push_back(ctx->id);
  };

  Ctx ctx1{&order, &mu, 1};
  Ctx ctx2{&order, &mu, 2};
  Ctx ctx3{&order, &mu, 3};

  xTimerStart(fn, &ctx3, 150, 0);
  xTimerStart(fn, &ctx1, 50, 0);
  xTimerStart(fn, &ctx2, 100, 0);

  /* Wait for all to fire */
  for (int i = 0; i < 10; i++) {
    xEventLoopRun(loop, X_RUN_ONCE);
    std::lock_guard<std::mutex> lock(mu);
    if (order.size() >= 3) break;
  }

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}


/* ───────────────────── Mixed I/O + Timer ───────────────────── */
/* ───────────────────── Mixed I/O + Timer ───────────────────── */

TEST(BuiltinTimerMixed, IOAndTimerTogether) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> io_count{0};
  std::atomic<int> timer_count{0};

  xEventSource src = xEventAdd(
    fds[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      static_cast<std::atomic<int> *>(arg)->fetch_add(1);
      drain_fd(fd);
    },
    &io_count);
  ASSERT_NE(src, nullptr);

  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &timer_count, 50, 0);

  /* Write data to trigger I/O */
  write_fd(fds[1], "x", 1);

  /* Wait for both */
  for (int i = 0; i < 10; i++) {
    xEventLoopRun(loop, X_RUN_ONCE);
    if (io_count.load() >= 1 && timer_count.load() >= 1) break;
  }

  EXPECT_GE(io_count.load(), 1);
  EXPECT_GE(timer_count.load(), 1);

  xEventDel(src);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}


/* ───────────────────── Run + Stop ───────────────────── */
/* ───────────────────── Run + Stop ───────────────────── */

TEST(BuiltinTimerRun, RunAndStop) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<bool> running{false};

  std::thread runner([&]() {
    running = true;
    xEventLoopRun(loop, X_RUN_DEFAULT);
  });

  while (!running)
    sleep_ms(1);
  sleep_ms(20);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerRun, NullDoesNotCrash) {
  xEventLoopRun(NULL, X_RUN_DEFAULT);
  xEventLoopStop(NULL);
}

TEST(BuiltinTimerRun, TimerFiresDuringRun) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 50, 0);

  std::thread runner([&]() { xEventLoopRun(loop, X_RUN_DEFAULT); });

  for (int i = 0; i < 40 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_GE(fired.load(), 1);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}


/* ───────────────────── Cross-thread timer submit ───────────────────── */
/* ───────────────────── Cross-thread timer submit ───────────────────── */

TEST(BuiltinTimerCrossThread, SubmitFromAnotherThread) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  /* Post timer start to loop thread so active_handles is incremented
   * before the loop checks loop_alive(). */
  struct Ctx { std::atomic<int> *fired; };
  auto *ctx = new Ctx{&fired};
  xEventLoopPost(loop,
    [](void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      xTimerStart([](void *a) { static_cast<std::atomic<int> *>(a)->fetch_add(1); },
                   c->fired, 50, 0);
      delete c;
    }, ctx);

  std::thread runner([&]() { xEventLoopRun(loop, X_RUN_DEFAULT); });

  for (int i = 0; i < 40 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_GE(fired.load(), 1);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}


/* ───────────────────── Destroy discards ───────────────────── */
/* ───────────────────── Destroy discards pending timers ─────────────────────
 */

TEST(BuiltinTimerDestroy, DiscardsPendingTimers) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  /* Schedule timers far in the future */
  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 10000, 0);
  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 20000, 0);

  /* Destroy without waiting — should not crash or fire callbacks */
  xEventLoopLeave();
  xEventLoopDestroy(loop);

  EXPECT_EQ(fired.load(), 0);
}


/* ───────────────────── Timer precision ───────────────────── */
/* ───────────────────── Timer precision ───────────────────── */

TEST(BuiltinTimerPrecision, DelayAccuracy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<uint64_t> fire_time{0};

  uint64_t submit_time = xMonoMs();
  xTimerStart([](void *arg) { static_cast<std::atomic<uint64_t> *>(arg)->store(xMonoMs()); },
    &fire_time, 100, 0);

  std::thread runner([&]() { xEventLoopRun(loop, X_RUN_DEFAULT); });

  for (int i = 0; i < 60 && fire_time.load() == 0; i++)
    sleep_ms(10);

  uint64_t ft = fire_time.load();
  ASSERT_NE(ft, (uint64_t)0);

  int64_t delay = (int64_t)(ft - submit_time) - 100;
  EXPECT_GE(delay, -5);
  EXPECT_LE(delay, 50);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}


/* ───────────────────── NowMs basic test ───────────────────── */
/* ───────────────────── NowMs basic test ───────────────────── */

TEST(BuiltinTimerNowMs, ReturnsNonZero) {
  uint64_t now = xMonoMs();
  EXPECT_GT(now, 0u);
}

TEST(BuiltinTimerNowMs, IsMonotonic) {
  uint64_t a = xMonoMs();
  sleep_ms(10);
  uint64_t b = xMonoMs();
  EXPECT_GE(b, a);
}

