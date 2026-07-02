#include <gtest/gtest.h>

#include <x/base/event.h>
#include <x/base/test_helper.h>

/* ───────────────────── TimerAfter ───────────────────── */
/* ───────────────────── TimerAfter ───────────────────── */

TEST(BuiltinTimerAfter, BasicDelay) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                         &fired, NULL, 50, 0);
  ASSERT_NE(t, nullptr);

  /* Wait long enough for the timer to fire (may need multiple waits
   * because the initial xEventLoopWake from TimerAfter can return early) */
  run_until_count(loop, fired, 1, 10000);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerAfter, ZeroDelayFiresImmediately) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, NULL,
              0, 0);

  xEventLoopRun(loop, X_RUN_ONCE);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerAfter, NullArgs) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  EXPECT_EQ(xTimerStart(nullptr, nullptr, NULL, 0, 0), nullptr);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── TimerAt ───────────────────── */
/* ───────────────────── TimerAt ───────────────────── */

TEST(BuiltinTimerAt, AbsoluteTime) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                         &fired, NULL, 50, 0);
  ASSERT_NE(t, nullptr);

  run_until_count(loop, fired, 1, 10000);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerAt, ExpiredDeadlineFiresImmediately) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  /* Deadline in the past */
  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, NULL,
              0, 0);

  xEventLoopRun(loop, X_RUN_ONCE);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── TimerCancel ───────────────────── */
/* ───────────────────── TimerCancel ───────────────────── */

TEST(BuiltinTimerCancel, CancelBeforeFire) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                         &fired, NULL, 500, 0);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xTimerStop(t), xErrno_Ok);

  /* Wait past the original deadline */
  xEventLoopRun(loop, X_RUN_ONCE);

  EXPECT_EQ(fired.load(), 0);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerCancel, NullArgs) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  EXPECT_EQ(xTimerStop(NULL), xErrno_InvalidArg);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── Cancel after fire ───────────────────── */
/* ───────────────────── Cancel after fire ───────────────────── */

TEST(BuiltinTimerCancel, CancelAfterFireReturnsError) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                         &fired, NULL, 10, 0);
  ASSERT_NE(t, nullptr);

  /* Wait for it to fire */
  run_until_count(loop, fired, 1, 10000);

  EXPECT_EQ(fired.load(), 1);

  /* Cancel after fire should return InvalidState */
  xErrno err = xTimerStop(t);
  EXPECT_EQ(err, xErrno_InvalidState);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── on_cancel (loop-destroy cleanup) ───────────── */

/* Combined state so one `arg` can be shared by fn and on_cancel. */
struct TimerCounters {
  std::atomic<int> fired{0};
  std::atomic<int> cancelled{0};
};

TEST(Timer, OnCancelFiresOnLoopDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  TimerCounters c;

  /* Long-delay timer that will NOT fire before destroy. */
  xTimer t = xTimerStart(
    [](void *arg) { static_cast<TimerCounters *>(arg)->fired.fetch_add(1); }, &c,
    [](void *arg) { static_cast<TimerCounters *>(arg)->cancelled.fetch_add(1); }, 100000, 0);
  ASSERT_NE(t, nullptr);
  (void)t;

  /* Don't wait for the timer — just leave and destroy. */
  xEventLoopLeave();
  xEventLoopDestroy(loop);

  EXPECT_EQ(c.fired.load(), 0);
  EXPECT_EQ(c.cancelled.load(), 1);
}

TEST(Timer, OnCancelNotInvokedOnStop) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  TimerCounters c;

  xTimer t = xTimerStart(
    [](void *arg) { static_cast<TimerCounters *>(arg)->fired.fetch_add(1); }, &c,
    [](void *arg) { static_cast<TimerCounters *>(arg)->cancelled.fetch_add(1); }, 500, 0);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xTimerStop(t), xErrno_Ok);

  /* Pump the loop once to ensure no deferred callback fires. */
  xEventLoopRun(loop, X_RUN_ONCE);

  EXPECT_EQ(c.fired.load(), 0);
  EXPECT_EQ(c.cancelled.load(), 0);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(Timer, OnCancelNotInvokedOnFire) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  TimerCounters c;

  xTimer t = xTimerStart(
    [](void *arg) { static_cast<TimerCounters *>(arg)->fired.fetch_add(1); }, &c,
    [](void *arg) { static_cast<TimerCounters *>(arg)->cancelled.fetch_add(1); }, 10, 0);
  ASSERT_NE(t, nullptr);

  run_until_count(loop, c.fired, 1, 10000);

  EXPECT_EQ(c.fired.load(), 1);
  EXPECT_EQ(c.cancelled.load(), 0);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(Timer, OnCancelNullIsNoOp) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  /* NULL on_cancel — destroy should not crash. */
  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
                         &fired, NULL, 100000, 0);
  ASSERT_NE(t, nullptr);
  (void)t;

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(Timer, OnCancelMultiplePending) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> cancelled{0};

  for (int i = 0; i < 5; i++) {
    xTimer t =
      xTimerStart([](void *) {}, &cancelled,
                  [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, 100000, 0);
    ASSERT_NE(t, nullptr);
    (void)t;
  }

  xEventLoopLeave();
  xEventLoopDestroy(loop);

  EXPECT_EQ(cancelled.load(), 5);
}

TEST(Timer, OnCancelRepeatingTimer) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  TimerCounters c;

  /* Repeating timer with a long interval; will not fire before destroy. */
  xTimer t = xTimerStart(
    [](void *arg) { static_cast<TimerCounters *>(arg)->fired.fetch_add(1); }, &c,
    [](void *arg) { static_cast<TimerCounters *>(arg)->cancelled.fetch_add(1); }, 100000, 100000);
  ASSERT_NE(t, nullptr);
  (void)t;

  xEventLoopLeave();
  xEventLoopDestroy(loop);

  EXPECT_EQ(c.fired.load(), 0);
  EXPECT_EQ(c.cancelled.load(), 1);
}
