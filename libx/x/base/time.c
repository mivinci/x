/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * time.c - Time utilities
 */

#include <x/base/time.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t xMonoNs(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER        count;

  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);

  QueryPerformanceCounter(&count);
  /* Convert to nanoseconds:
   *   count / freq * 1e9  =>  (count * 1e9) / freq */
  return (uint64_t)(count.QuadPart / freq.QuadPart) * 1000000000u +
         (uint64_t)((count.QuadPart % freq.QuadPart) * 1000000000u / freq.QuadPart);
}

uint64_t xMonoMs(void) {
  return xMonoNs() / 1000000u;
}

uint64_t xWallMs(void) {
  FILETIME       ft;
  ULARGE_INTEGER ul;

  GetSystemTimeAsFileTime(&ft);
  ul.LowPart  = ft.dwLowDateTime;
  ul.HighPart = ft.dwHighDateTime;

  /* FILETIME is 100-ns intervals since 1601-01-01 UTC.
   * Unix epoch (1970-01-01) is 11644473600 seconds later.
   * Convert to ms: (ticks / 10000) - epoch_offset_ms */
  return (ul.QuadPart / 10000u) - 11644473600000uLL;
}

#else /* POSIX */

#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <x/base/thread.h>

static xOnce                     tb_init = X_ONCE_INIT;
static mach_timebase_info_data_t tb;

static void timebase_init(void) {
  mach_timebase_info(&tb);
}

uint64_t xMonoNs(void) {
  xOnceCall(&tb_init, timebase_init);

  /* mach_continuous_time() does not pause during system sleep,
   * unlike mach_absolute_time().  Aligned with libuv's uv__hrtime. */
  uint64_t ticks = mach_continuous_time();
  return ticks * tb.numer / tb.denom;
}

uint64_t xMonoMs(void) {
  return xMonoNs() / 1000000u;
}

#else /* Linux / BSD */

uint64_t xMonoNs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint64_t xMonoMs(void) {
  return xMonoNs() / 1000000u;
}

#endif /* __APPLE__ */

uint64_t xWallMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

#endif /* _WIN32 / POSIX */
