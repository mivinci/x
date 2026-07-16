/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * relay.c - 1:N fan-out pub/sub implementation
 *
 * See relay.h for the public API and design rationale.
 *
 * Internal structures:
 *
 *   xRelaySub_    — a subscriber: {loop, fn, arg} embedded in a list.
 *   xRelayDispatch_ — cross-loop delivery: owns a copy of data+metadata.
 *   xRelay_       — the relay itself: subscriber list + mutex.
 *
 * Concurrency notes:
 *
 *   - xRelayOn / xRelayOff take the mutex, modify the subscriber list.
 *   - xRelayEmit takes the mutex ONLY to snapshot subscriber pointers
 *     into a stack array.  Callback execution runs lock-free.
 *   - For cross-loop dispatch we heap-allocate a xRelayDispatch_ that
 *     owns a copy of the data.  The dispatch callback frees it.
 *   - xRelayDestroy takes the mutex, drains the list, then tears down.
 */

#include <stdlib.h>
#include <string.h>

#include <x/base/relay.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Internal types
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Subscriber node — embedded in xRelay_->subs linked list.
 *
 * The @p loop field is a snapshot of xEventLoopCurrent() captured at
 * xRelayOn() time.  It does not change for the lifetime of the
 * subscriber even if the owning thread switches event loops — the
 * relay has no way to track such changes, and the pattern is rare
 * enough that we don't try to support it.
 */
typedef struct xRelaySub_ {
  xList      node;  /**< embedded list node */
  xEventLoop loop;  /**< snapshot from xRelayOn() */
  xRelayFunc fn;    /**< subscriber callback */
  void      *arg;   /**< opaque user pointer */
} xRelaySub_;

/**
 * @brief Cross-loop dispatch payload.
 *
 * Allocated by xRelayEmit for subscribers on a different event
 * loop.  The dispatch_fn callback invokes the subscriber, then
 * frees both the data copy and this struct.
 */
typedef struct xRelayDispatch_ {
  xRelayFunc fn;    /**< subscriber callback (copied from xRelaySub_) */
  void      *arg;   /**< opaque user pointer (copied from xRelaySub_) */
  void      *data;  /**< payload copy — heap-allocated by xRelayEmit */
} xRelayDispatch_;

/**
 * @brief The relay itself.
 *
 * Only two members: a doubly-linked list of subscribers, and a
 * mutex that serialises On/Off mutations.
 */
struct xRelay_ {
  xList  subs;   /**< subscriber list head */
  xMutex lock;   /**< protects @p subs against concurrent On/Off */
};

/* ═══════════════════════════════════════════════════════════════════
 *  Static helpers
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Callback invoked on the subscriber's event loop for
 *        cross-loop dispatch.
 *
 * Fires the subscriber, then frees both the data copy and the
 * dispatch struct itself.  One-shot — never called more than once.
 */
static void dispatch_fn(void *arg) {
  xRelayDispatch_ *d = (xRelayDispatch_ *)arg;
  d->fn(d->data, d->arg);
  free(d->data);
  free(d);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

xRelay *xRelayCreate(void) {
  xRelay *r = (xRelay *)calloc(1, sizeof(xRelay));
  if (!r) return NULL;

  xListInit(&r->subs);
  xMutexInit(&r->lock);
  return r;
}

xErrno xRelayOn(xRelay *r, xRelayFunc fn, void *arg) {
  xRelaySub_ *sub = (xRelaySub_ *)calloc(1, sizeof(xRelaySub_));
  if (!sub) return xErrno_NoMemory;

  sub->loop = xEventLoopCurrent(); /* may be NULL — xRelayEmit handles it */
  sub->fn   = fn;
  sub->arg  = arg;

  xMutexLock(&r->lock);
  xListAddTail(&r->subs, &sub->node);
  xMutexUnlock(&r->lock);
  return xErrno_Ok;
}

void xRelayOff(xRelay *r, xRelayFunc fn, void *arg) {
  xMutexLock(&r->lock);

  xList *pos, *tmp;
  xListForEachSafe(pos, tmp, &r->subs) {
    xRelaySub_ *sub = xContainerOf(pos, xRelaySub_, node);
    if (sub->fn == fn && sub->arg == arg) {
      xListDel(pos);
      xMutexUnlock(&r->lock);
      free(sub);
      return; /* only remove the first match */
    }
  }

  xMutexUnlock(&r->lock);
}

void xRelayEmit(xRelay *r, const void *data, size_t size) {
  xEventLoop cur = xEventLoopCurrent();

  /*
   * ── Phase 1: snapshot the subscriber list under the mutex ──
   *
   * We count subscribers first, allocate a pointer array (stack for
   * the common ≤16 case, heap otherwise), then fill it.  The two
   * passes over the list are cheap — no allocation, no I/O, no
   * callback execution inside the critical section.
   */

  /* Count subscribers. */
  int n = 0;
  xMutexLock(&r->lock);
  xList *pos;
  xListForEach(pos, &r->subs) {
    n++;
  }

  if (n == 0) {
    xMutexUnlock(&r->lock);
    return; /* nothing to do — avoid allocating a zero-element array */
  }

  /* Allocate snapshot array.  Stack allocation for the common case. */
  xRelaySub_ *stack[16];
  xRelaySub_ **snap = (n <= 16)
                        ? stack
                        : (xRelaySub_ **)calloc((size_t)n, sizeof(xRelaySub_ *));
  if (!snap) {
    xMutexUnlock(&r->lock);
    return;
  }

  /* Fill the snapshot. */
  int i = 0;
  xListForEach(pos, &r->subs)
    snap[i++] = xContainerOf(pos, xRelaySub_, node);
  xMutexUnlock(&r->lock);

  /*
   * ── Phase 2: dispatch outside the mutex ──
   *
   * Same-loop / no-loop subscribers are called synchronously —
   * the data pointer is still valid on the caller's stack.
   * Cross-loop subscribers get a heap copy dispatched via
   * xEventLoopPost (which internally calls xEventLoopWake).
   */

  for (int j = 0; j < n; j++) {
    xRelaySub_ *sub = snap[j];

    if (sub->loop == cur || sub->loop == NULL) {
      /*
       * Same loop or no loop recorded:
       *   - Data lives on the caller's stack — zero-copy, zero-allocation.
       *   - The callback runs synchronously on the publisher's stack.
       */
      sub->fn((void *)data, sub->arg);
    } else {
      /*
       * Different event loop — defer delivery.
       *
       * We must copy the data because the publisher may return (and
       * free its buffer) long before the subscriber's loop iterates.
       * The dispatch_fn callback will free both the data copy and
       * this dispatch struct after invoking the subscriber.
       */
      xRelayDispatch_ *d = (xRelayDispatch_ *)calloc(1, sizeof(xRelayDispatch_));
      if (!d) { continue; }

      d->fn  = sub->fn;
      d->arg = sub->arg;
      d->data = NULL;

      if (size > 0 && data != NULL) {
        d->data = malloc(size);
        if (!d->data) { free(d); continue; }
        memcpy(d->data, data, size);
      }

      /*
       * xEventLoopPost internally calls xEventLoopWake, so the
       * target loop will drain the done queue on its next iteration.
       */
      xEventLoopPost(sub->loop, dispatch_fn, d);
    }
  }

  if (snap != stack) free(snap);
}

void xRelayDestroy(xRelay *r) {
  /*
   * Drain and free all subscribers under the mutex first — this
   * ensures no concurrent xRelayOn / xRelayOff can race with the
   * teardown.
   */
  xMutexLock(&r->lock);
  while (!xListEmpty(&r->subs)) {
    xRelaySub_ *sub = xContainerOf(r->subs.next, xRelaySub_, node);
    xListDel(&sub->node);
    free(sub);
  }
  xMutexUnlock(&r->lock);

  xMutexDestroy(&r->lock);
  free(r);
}
