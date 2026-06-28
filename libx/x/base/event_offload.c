/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_offload.c - Async offload: submit work to a thread pool,
 *                   deliver completion callback on the event loop thread.
 */

#include "event_private.h"

#include <stdlib.h>

/* ───────────────────── Worker wrapper ───────────────────── */

static void *offload_worker(void *arg) {
  struct xWork_ *w = (struct xWork_ *)arg;

  /* Execute the user's work function on the worker thread. */
  w->result = w->work_fn(w->arg);

  /* Enqueue the work item into the done queue (lock-free). */
  xMpscPush(&((struct xEventLoop_ *)w->loop)->done_head,
            &((struct xEventLoop_ *)w->loop)->done_tail, &w->mpsc);

  /*
   * Wake the event loop so it drains the done queue promptly.
   *
   * Return value intentionally ignored: EAGAIN means the pipe already
   * has data so the loop will wake anyway; a real failure (closed fd)
   * is a bug in the caller.  Either way the done-queue item is not
   * lost — it will be picked up on the next loop iteration.
   */
  xEventLoopWake(w->loop);

  return NULL;
}

/* ───────────────────── Public API ───────────────────── */

xWork xWorkSubmit(xTaskGroup group, xTaskFunc work_fn,
                       xWorkDoneFunc done_fn, xWorkCancelFunc on_cancel,
                       void *arg) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)xEventLoopCurrent();
  if (!loop || !work_fn) return NULL;

  if (!group) {
    xTaskGroup g = loop->task_group;
    if (!g) g = xTaskGroupGlobal();
    if (!g) return NULL;
    group = g;
  }

  struct xWork_ *w = event_work_alloc((struct xEventLoop_ *)loop);
  if (!w) return NULL;

  w->work_fn   = work_fn;
  w->done_fn   = done_fn;
  w->on_cancel = on_cancel;
  w->arg       = arg;
  w->result  = NULL;
  w->loop    = (xEventLoop)loop;

  xTask t = xTaskSubmit(group, offload_worker, w);
  if (!t) {
    event_work_free((struct xEventLoop_ *)loop, w);
    return NULL;
  }

  w->task = t;
  xAtomicFetchAdd(&((struct xEventLoop_ *)loop)->inflight, 1, xAtomicRelaxed);

  return (xWork)w;
}

xErrno xWorkCancel(xWork work) {
  if (!work) return xErrno_InvalidArg;

  struct xWork_ *w = (struct xWork_ *)work;
  struct xEventLoop_ *loop = (struct xEventLoop_ *)xEventLoopCurrent();

  /* The work item must belong to this loop. */
  if (w->loop != (xEventLoop)loop) return xErrno_InvalidContext;

  /* Mark as cancelled — done_fn will be skipped regardless of task state. */
  w->cancelled = 1;

  /* Attempt to cancel the underlying task. If successful, the
   * offload_worker will never execute and we must push the work item
   * to the done queue ourselves for cleanup. */
  xErrno err = xTaskCancel(w->task);
  if (err == xErrno_Ok) {
    w->result = NULL;
    struct xEventLoop_ *l = (struct xEventLoop_ *)xEventLoopCurrent();
    xMpscPush(&l->done_head, &l->done_tail, &w->mpsc);
    xEventLoopWake(w->loop);
  }

  return xErrno_Ok;
}
