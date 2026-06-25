/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_post.c - Post a callback directly to the event loop thread
 *                without involving a thread pool.
 */

#include "event_private.h"

xErrno xEventLoopPost(xEventLoop loop_, xEventLoopPostFunc fn, void *arg) {
  struct xEventLoop_ *l = (struct xEventLoop_ *)loop_;
  if (!l || !fn) return xErrno_InvalidArg;

  struct xWork_ *w = event_work_alloc(l);
  if (!w) return xErrno_NoMemory;

  w->post_fn = fn;
  w->arg     = arg;
  w->result  = NULL;
  w->loop    = (xEventLoop)l;
  w->task    = NULL; /* distinguishes post from offload */

  /* Enqueue into the done queue (lock-free). */
  xMpscPush(&l->done_head, &l->done_tail, &w->mpsc);

  /* Wake the event loop so it drains the queue promptly. */
  xEventLoopWake((xEventLoop)l);

  return xErrno_Ok;
}
