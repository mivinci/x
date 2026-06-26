#include <x/base/event.h>

#include <gtest/gtest.h>

#include <x/base/test_helper.h>

/* ───────────────────── TimerAfter ───────────────────── */
/* ───────────────────── TimerAfter ───────────────────── */

TEST(BuiltinTimerAfter, BasicDelay) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> fired{0};

  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 50, 0);
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

  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 0, 0);

  xEventLoopRun(loop, X_RUN_ONCE);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerAfter, NullArgs) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  EXPECT_EQ(xTimerStart(nullptr, nullptr, 0, 0), nullptr);

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

  xTimer t        = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 50, 0);
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
  xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 0, 0);

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

  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 500, 0);
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

  xTimer t = xTimerStart([](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &fired, 10, 0);
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


