/* event_timer_precision_test.cpp — Precision and load tests */
#include "event_timer_test_helpers.h"

#include <cmath>
#include <vector>

/* ───────────────────── Done queue flood ───────────────────── */

TEST(BuiltinTimerPrecision, UnderDoneQueueFlood) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> posted_count{0};
  for (int i = 0; i < 1000; i++)
    xEventLoopPost(loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); }, &posted_count);

  std::atomic<uint64_t> fire_time{0};
  xTimerStart([](void *arg) { static_cast<std::atomic<uint64_t> *>(arg)->store(xMonoNs()); }, &fire_time, 100, 0);

  uint64_t start_ns = xMonoNs();
  for (int i = 0; i < 300 && fire_time.load() == 0; i++)
    xEventLoopRun(loop, X_RUN_ONCE);

  ASSERT_GT(fire_time.load(), 0u) << "timer should have fired";
  uint64_t elapsed_ns = fire_time.load() - start_ns;
  EXPECT_GE(elapsed_ns, 80u * 1000000u) << "timer too early";
  EXPECT_LE(elapsed_ns, 130u * 1000000u) << "timer delayed";

  while (posted_count.load() < 1000 && xMonoNs() - start_ns < 2000u * 1000000u)
    xEventLoopRun(loop, X_RUN_ONCE);
  EXPECT_EQ(posted_count.load(), 1000) << "all posted callbacks should eventually run";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── Fd flood ───────────────────── */

TEST(BuiltinTimerPrecision, UnderFdFlood) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int fds[2]; ASSERT_EQ(make_pipe(fds), 0);
  std::atomic<int> fd_count{0};
  xEventAdd(fds[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      auto *c = static_cast<std::atomic<int> *>(arg);
      char buf[64];
      while (read(fd, buf, sizeof(buf)) > 0) c->fetch_add(1);
    }, &fd_count);

  const char *msg = "x";
  for (int i = 0; i < 2000; i++) (void)write(fds[1], msg, 1);

  std::atomic<uint64_t> fire_time{0};
  xTimerStart([](void *arg) { static_cast<std::atomic<uint64_t> *>(arg)->store(xMonoNs()); }, &fire_time, 100, 0);

  uint64_t start_ns = xMonoNs();
  {
    xTimer stop = xTimerStart([](void *arg) { xEventLoopStop((xEventLoop)arg); }, loop, 200, 0);
    xEventLoopRun(loop, X_RUN_DEFAULT);
    if (stop) xTimerStop(stop);
  }
  ASSERT_GT(fire_time.load(), 0u);
  uint64_t elapsed_ns = fire_time.load() - start_ns;
  EXPECT_GE(elapsed_ns, 80u * 1000000u);
  EXPECT_LE(elapsed_ns, 150u * 1000000u);

  close_fd(fds[0]); close_fd(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── Heavy callbacks ───────────────────── */

TEST(BuiltinTimerPrecision, UnderHeavyCallbacks) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  std::atomic<int> heavy_count{0};
  for (int i = 0; i < 200; i++)
    xEventLoopPost(loop,
      [](void *arg) {
        static_cast<std::atomic<int> *>(arg)->fetch_add(1);
        usleep(2000);
      }, &heavy_count);

  std::atomic<uint64_t> fire_time{0};
  xTimerStart([](void *arg) { static_cast<std::atomic<uint64_t> *>(arg)->store(xMonoNs()); }, &fire_time, 100, 0);

  /* Pump until the timer fires.  X_RUN_ONCE processes at most
   * 2*EVENT_DONE_BATCH_MAX done-callbacks per iteration, so with 200
   * posted items multiple iterations are needed before the timer's
   * 100ms deadline is reached. */
  uint64_t start_ns = xMonoNs();
  for (int i = 0; i < 500 && fire_time.load() == 0; i++)
    xEventLoopRun(loop, X_RUN_ONCE);
  ASSERT_GT(fire_time.load(), 0u) << "timer should have fired";
  uint64_t elapsed_ns = fire_time.load() - start_ns;
  EXPECT_GE(elapsed_ns, 80u * 1000000u);
  /* Upper bound is generous: on slow CI runners usleep(2000) can take
   * 10-20ms per call, so draining 200 callbacks can add seconds. */
  EXPECT_LE(elapsed_ns, 2000u * 1000000u);

  /* Drain remaining callbacks. */
  for (int i = 0; i < 500 && heavy_count.load() < 200; i++)
    xEventLoopRun(loop, X_RUN_ONCE);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── Heavy fd callbacks ───────────────────── */

TEST(BuiltinTimerPrecision, UnderHeavyFdCallbacks) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  std::atomic<int> fd_count{0};

  xEventAdd(fds[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      auto *c   = static_cast<std::atomic<int> *>(arg);
      char  buf[1];
      (void)read(fd, buf, 1);
      c->fetch_add(1);
      usleep(2000);
    },
    &fd_count);

  /* Pre-fill the pipe with 100 bytes.  Each byte triggers one callback. */
  const char c = 'x';
  for (int i = 0; i < 100; i++)
    (void)write(fds[1], &c, 1);

  std::atomic<uint64_t> fire_time{0};
  xTimerStart([](void *arg) { static_cast<std::atomic<uint64_t> *>(arg)->store(xMonoNs()); }, &fire_time, 100, 0);

  uint64_t start_ns = xMonoNs();
  for (int i = 0; i < 500 && fire_time.load() == 0; i++)
    xEventLoopRun(loop, X_RUN_ONCE);

  ASSERT_GT(fire_time.load(), 0u) << "timer should have fired";

  uint64_t elapsed_ns = fire_time.load() - start_ns;
  printf("[heavy-fd] fd_callbacks=%d elapsed=%.3f ms\n",
         fd_count.load(), elapsed_ns / 1e6);

  /* 100 fd callbacks × 2 ms = 200 ms of inline work.  Timer fires between
   * kevent calls, so delay ≤ 1 batch of 64 events × 2 ms ≈ 128 ms. */
  EXPECT_GE(elapsed_ns, 80u * 1000000u);
  EXPECT_LE(elapsed_ns, 250u * 1000000u);

  close_fd(fds[0]);
  close_fd(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── Jitter analysis ───────────────────── */

TEST(BuiltinTimerPrecision, JitterAnalysis) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  const int N = 200;
  std::vector<uint64_t> fire_times(N, 0);
  const uint64_t expected_ns = xMonoNs() + 100u * 1000000u;

  for (int i = 0; i < N; i++)
    xTimerStart(
      [](void *arg) { *static_cast<uint64_t *>(arg) = xMonoNs(); },
      &fire_times[i], 100, 0);

  for (int i = 0; i < 200; i++) {
    xEventLoopRun(loop, X_RUN_ONCE);
    int done = 0; for (int j = 0; j < N; j++) if (fire_times[j] != 0) done++;
    if (done >= N) break;
  }

  int actual_fires = 0;
  double min_jitter_ns = 1e18, max_jitter_ns = -1e18, sum = 0, sum_sq = 0;
  for (int i = 0; i < N; i++) {
    if (fire_times[i] == 0) continue;
    actual_fires++;
    double j = (double)((int64_t)(fire_times[i] - expected_ns));
    min_jitter_ns = std::min(min_jitter_ns, j);
    max_jitter_ns = std::max(max_jitter_ns, j);
    sum += j; sum_sq += j * j;
  }
  double mean_ns   = actual_fires > 0 ? sum / actual_fires : 0;
  double stddev_ns = actual_fires > 1 ? std::sqrt((sum_sq - sum * sum / actual_fires) / (actual_fires - 1)) : 0;

  printf("[timer-jitter] N=%d fires=%d min=%.0f ns max=%.0f ns mean=%.1f ns stddev=%.1f ns  "
         "(%.3f ms / %.3f ms)\n",
         N, actual_fires, min_jitter_ns, max_jitter_ns, mean_ns, stddev_ns,
         mean_ns / 1e6, stddev_ns / 1e6);

  EXPECT_GE(actual_fires, N * 95 / 100);
  EXPECT_LE(stddev_ns, 5e6) << "jitter stddev should be < 5 ms (in ns)";

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

