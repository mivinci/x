/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_signal.h - Shared self-pipe signal handler (epoll, poll backends)
 *
 * kqueue uses native EVFILT_SIGNAL and does not include this header.
 */

#ifndef XBASE_EVENT_SIGNAL_H
#define XBASE_EVENT_SIGNAL_H

#include <signal.h>
#include <unistd.h>

#define X_SIGNAL_MAX 64

/*
 * Self-pipe trick for signal delivery.
 *
 * kqueue has EVFILT_SIGNAL — the kernel queues signal events directly,
 * no userspace pipe needed.  epoll and poll don't have that, so we
 * install a normal signal handler via sigaction(2) which writes a byte
 * to a per-signal pipe.  The pipe's read end is registered with epoll
 * (or rebuilt into the pollfds array for poll), and the loop thread
 * drains it during poll().
 *
 * signal_handler() is async-signal-safe: write(2) is on the safe list.
 * The handler writes the signal number as a single byte to the pipe.
 * Multiple deliveries between poll() calls are coalesced — the byte
 * only marks "signal N arrived", not how many times.
 */

#if !defined(X_HAS_KQUEUE)
/* kqueue doesn't need these — it uses EVFILT_SIGNAL. */
static volatile int g_signal_pipe_w[X_SIGNAL_MAX] = {[0 ... X_SIGNAL_MAX - 1] = -1};

static void signal_handler(int signo) {
  if (signo > 0 && signo < X_SIGNAL_MAX) {
    int wfd = g_signal_pipe_w[signo];
    if (wfd >= 0) {
      char c = (char)signo;
      (void)write(wfd, &c, 1);
    }
  }
}
#endif /* !X_HAS_KQUEUE */

#endif /* XBASE_EVENT_SIGNAL_H */
