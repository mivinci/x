/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_timer.c - Builtin event loop timer API
 */

#include "event_private.h"
#include <stdlib.h>

static xTimer submit_timer(xTimerFunc fn, void *arg, uint64_t abs_ms, uint64_t repeat_ms) {
  xEventLoop         loop_ = xEventLoopCurrent();
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop || !fn) return NULL;
  struct xTimer_ *t = timer_alloc(loop);
  if (!t) return NULL;
  t->deadline  = abs_ms;
  t->fn        = fn;
  t->arg       = arg;
  t->heap_idx  = TIMER_INVALID_IDX;
  t->fired     = 0;
  t->loop      = loop_;
  t->repeat_ms = repeat_ms;

  loop->active_handles++;
  if (xHeapPush(loop->timer_heap, t) != xErrno_Ok) {
    timer_free(loop, t);
    return NULL;
  }
  xEventLoopWake(loop_);
  return (xTimer)t;
}

xTimer xTimerStart(xTimerFunc fn, void *arg, uint64_t timeout_ms, uint64_t repeat_ms) {
  return submit_timer(fn, arg, xMonoMs() + timeout_ms, repeat_ms);
}

/* ── Stop ── */

static xErrno timer_stop_direct(xTimer timer_) {
  struct xTimer_    *timer = (struct xTimer_ *)timer_;
  struct xEventLoop_ *loop;
  if (!timer) return xErrno_InvalidArg;
  loop = (struct xEventLoop_ *)timer->loop;
  if (timer->fired || timer->heap_idx == TIMER_INVALID_IDX)
    return xErrno_InvalidState;
  xHeapRemove(loop->timer_heap, timer->heap_idx);
  timer->heap_idx = TIMER_INVALID_IDX;
  timer_free(loop, timer);
  return xErrno_Ok;
}

static void timer_stop_post(void *arg) {
  struct { xEventLoop loop; xTimer timer; } *ctx = arg;
  timer_stop_direct(ctx->timer);
  free(ctx);
}

xErrno xTimerStop(xTimer timer_) {
  struct xTimer_ *timer = (struct xTimer_ *)timer_;
  struct { xEventLoop loop; xTimer timer; } *ctx;
  if (!timer) return xErrno_InvalidArg;
  if (xEventLoopCurrent() != timer->loop) {
    ctx = malloc(sizeof(*ctx));
    if (!ctx) return xErrno_NoMemory;
    ctx->loop  = timer->loop;
    ctx->timer = timer_;
    xEventLoopEnter(timer->loop);
    xEventLoopPost(timer->loop, timer_stop_post, ctx);
    xEventLoopLeave();
    return xErrno_Ok;
  }
  return timer_stop_direct(timer_);
}
