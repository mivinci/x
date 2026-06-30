/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_helper.h - Shared event-loop test utilities for all modules.
 *
 * Provides run_for / run_until / run_until_count — X_RUN_DEFAULT-based
 * replacements for the fragile pump_loop / pump_until busy-loop patterns.
 * The loop sleeps efficiently in poll, exits immediately on completion,
 * and a watchdog timer prevents hangs.
 */

#ifndef XBASE_TEST_HELPER_H
#define XBASE_TEST_HELPER_H

#include <atomic>

#include <x/base/event.h>

/**
 * @brief Run the event loop for a fixed duration.
 *
 * Uses X_RUN_DEFAULT with a one-shot stop timer, so the loop sleeps
 * efficiently in poll instead of busy-looping.
 */
static inline void run_for(xEventLoop loop, int ms) {
  xTimer t = xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop,
                         static_cast<uint64_t>(ms), 0);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  if (t) xTimerStop(t);
}

/**
 * @brief Run the event loop until @p flag becomes true or timeout expires.
 *
 * A repeating timer polls the flag every 5 ms and stops the loop when it
 * is set.  A one-shot watchdog timer stops the loop after @p timeout_ms to
 * prevent hangs on failure.
 */
static inline void run_until(xEventLoop loop, std::atomic<bool> &flag, int timeout_ms = 5000) {
  if (flag.load(std::memory_order_acquire)) return;

  struct RunUntilCtx {
    xEventLoop         loop;
    std::atomic<bool> *flag;
  } ctx{loop, &flag};

  xTimer checker = xTimerStart(
    [](void *arg) {
      auto *c = static_cast<RunUntilCtx *>(arg);
      if (c->flag->load(std::memory_order_acquire)) xEventLoopStop(c->loop);
    },
    &ctx, 5, 5);

  xTimer watchdog = xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); },
                                loop, static_cast<uint64_t>(timeout_ms), 0);

  xEventLoopRun(loop, X_RUN_DEFAULT);

  if (checker) xTimerStop(checker);
  if (watchdog) xTimerStop(watchdog);
}

/**
 * @brief Run the event loop until @p count reaches @p target or timeout.
 *
 * Same efficient X_RUN_DEFAULT pattern as run_until, but for integer
 * counters (e.g. event counts).
 */
static inline void run_until_count(xEventLoop loop, std::atomic<int> &count, int target,
                                   int timeout_ms = 10000) {
  if (count.load(std::memory_order_acquire) >= target) return;

  struct RunUntilCountCtx {
    xEventLoop        loop;
    std::atomic<int> *count;
    int               target;
  } ctx{loop, &count, target};

  xTimer checker = xTimerStart(
    [](void *arg) {
      auto *c = static_cast<RunUntilCountCtx *>(arg);
      if (c->count->load(std::memory_order_acquire) >= c->target) xEventLoopStop(c->loop);
    },
    &ctx, 5, 5);

  xTimer watchdog = xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); },
                                loop, static_cast<uint64_t>(timeout_ms), 0);

  xEventLoopRun(loop, X_RUN_DEFAULT);

  if (checker) xTimerStop(checker);
  if (watchdog) xTimerStop(watchdog);
}

#endif /* XBASE_TEST_HELPER_H */
