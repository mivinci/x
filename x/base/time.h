/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * time.h - Time utilities
 */

#ifndef XBASE_TIME_H
#define XBASE_TIME_H

#include <stdint.h>
#include <x/base/base.h>

/**
 * @brief Return the current monotonic time in nanoseconds (CLOCK_MONOTONIC).
 *
 * High-resolution monotonic clock.  mach_continuous_time() on Apple,
 * clock_gettime(CLOCK_MONOTONIC) elsewhere.
 *
 * @return Current monotonic time in nanoseconds.
 */
XCAPI(uint64_t) xMonoNs(void);

/**
 * @brief Return the current monotonic time in milliseconds (CLOCK_MONOTONIC).
 *
 * Convenience wrapper: xMonoNs() / 1_000_000.
 * Suitable for measuring elapsed time, timeouts, and intervals.
 *
 * @return Current monotonic time in milliseconds.
 */
XCAPI(uint64_t) xMonoMs(void);

/**
 * @brief Return the current wall-clock time in milliseconds (CLOCK_REALTIME).
 *
 * Named "Wall" after the common term "wall clock". Unlike xMonoMs(), this
 * clock may jump forward or backward due to NTP adjustments or manual changes.
 * Use this only when you need calendar/epoch time (e.g. timestamps in logs).
 *
 * @return Current wall-clock time in milliseconds since the Unix epoch.
 */
XCAPI(uint64_t) xWallMs(void);

#endif /* XBASE_TIME_H */
