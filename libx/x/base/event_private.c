/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_private.c - Non-inline implementations for event loop internals
 */

#include "event_private.h"

/* ───────────────────── Source array ───────────────────── */

void source_array_free(struct xEventSourceArray_ *s) {
  for (size_t i = 0; i < s->len; i++)
    free(s->items[i]);
  free(s->items);
  s->items = NULL;
  s->len   = 0;
  s->cap   = 0;
}

struct xEventSource_ *source_array_add(struct xEventSourceArray_ *s, int fd, xEventMask mask,
                                       xEventFunc fn, void *arg) {
  if (s->len == s->cap) {
    size_t                 newcap = s->cap ? s->cap * 2 : 16;
    struct xEventSource_ **tmp =
      (struct xEventSource_ **)realloc(s->items, newcap * sizeof(*s->items));
    if (!tmp) return NULL;
    s->items = tmp;
    s->cap   = newcap;
  }
  struct xEventSource_ *src = (struct xEventSource_ *)calloc(1, sizeof(struct xEventSource_));
  if (!src) return NULL;
  src->fd            = fd;
  src->mask          = mask;
  src->fn            = fn;
  src->arg           = arg;
  s->items[s->len++] = src;
  return src;
}

/**
 * @brief Sweep deleted sources after a dispatch batch completes.
 *
 * Must be called at the end of each event loop iteration to actually free
 * sources that were removed during callback dispatch.
 */
void source_array_sweep(struct xEventSourceArray_ *s) {
  size_t i = 0;
  while (i < s->len) {
    if (s->items[i]->deleted) {
      free(s->items[i]);
      s->items[i] = s->items[--s->len];
    } else {
      i++;
    }
  }
}

/* ───────────────────── Timer firing ───────────────────── */

/*
 * Pop all expired timers under a single lock acquisition, then fire
 * them and return each struct to the pool.  Replaces the per-pop
 * lock/unlock pattern in all three backends.
 */
int loop_run_timers(struct xEventLoop_ *loop) {
  uint64_t now = loop->time;
  /* Pop all expired timers into a ready queue (intrusive linked list).
   * This avoids batch-array malloc/realloc management and eliminates
   * use-after-free when one callback cancels another. */
  xList ready;
  xListInit(&ready);

  while (xHeapSize(loop->timer_heap) > 0) {
    struct xTimer_ *t = (struct xTimer_ *)xHeapPeek(loop->timer_heap);
    if (t->deadline > now) break;
    xHeapPop(loop->timer_heap);
    t->fired     = 1;
    t->heap_idx  = TIMER_INVALID_IDX; /* needed for rearm / free check */
    xListAddTail(&ready, &t->ready_node);
  }

  int dispatched = 0;
  struct xTimer_ *t, *tmp;
  xListForEachEntrySafe(t, tmp, &ready, ready_node) {
    xListDel(&t->ready_node);
    /* Re-arm or recycle BEFORE firing the callback. */
    if (t->repeat_ms > 0 && t->heap_idx == TIMER_INVALID_IDX) {
      t->deadline += t->repeat_ms;
      t->fired    = 0;
      xHeapPush(loop->timer_heap, t);
    } else if (t->heap_idx == TIMER_INVALID_IDX) {
      timer_free(loop, t);
    }
    t->fn(t->arg);
    dispatched++;
  }

  return dispatched;
}

/* ───────────────────── Done queue dispatch ───────────────────── */

/* Dispatch completed work items (offload + post) from the done queue.
 * Processes at most max_batch items; returns the number dispatched.
 * Pass -1 for max_batch to drain the entire queue (used during destroy). */
int loop_run_done(struct xEventLoop_ *loop, int max_batch) {
  int    dispatched = 0;
  xMpsc *node;
  do {
    if (max_batch >= 0 && dispatched >= max_batch) break;
    node = xMpscPop(&loop->done_head, &loop->done_tail);
    if (!node) break;
    struct xWork_ *w = xContainerOf(node, struct xWork_, mpsc);
    if (w->task) {
      xErrno err = xTaskWait(w->task, NULL);
      if (err != xErrno_Cancelled && w->done_fn) w->done_fn(w->arg, w->result);
      xAtomicFetchSub(&loop->inflight, 1, xAtomicRelaxed);
    } else {
      if (w->post_fn) w->post_fn(w->arg);
    }
    event_work_free(loop, w);
    dispatched++;
  } while (1);
  return dispatched;
}

/* Drain remaining work items without executing callbacks (for destroy). */
void loop_cleanup_done(struct xEventLoop_ *loop) {
  xMpsc *node;
  while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
    struct xWork_ *w = xContainerOf(node, struct xWork_, mpsc);
    if (w->task) xTaskWait(w->task, NULL);
    free(w); /* truly free — loop is being destroyed */
  }
}

/*
 * Spin-wait until all in-flight offload workers have finished and
 * pushed their results into the done queue.  Must be called before
 * loop_cleanup_done() during destroy to avoid use-after-free.
 *
 * We drain the done queue inside the spin loop: inflight is only
 * decremented when a completed work item is popped, so without
 * draining the loop would spin forever if destroy happens while
 * offload work is still in-flight (worker has pushed to the done
 * queue but loop_run_done hasn't run yet).  done_fn is NOT fired —
 * the loop is being torn down.
 */
void loop_wait_inflight(struct xEventLoop_ *loop) {
  while (xAtomicLoad(&loop->inflight, xAtomicAcquire) > 0) {
    xMpsc *node;
    while ((node = xMpscPop(&loop->done_head, &loop->done_tail)) != NULL) {
      struct xWork_ *w = xContainerOf(node, struct xWork_, mpsc);
      if (w->task) {
        xTaskWait(w->task, NULL);
        xAtomicFetchSub(&loop->inflight, 1, xAtomicRelaxed);
      }
      free(w);
    }
    /* Brief yield to let worker threads finish. */
#ifdef _WIN32
    Sleep(0);
#else
    usleep(100);
#endif
  }
}
