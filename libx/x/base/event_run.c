/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_run.c - Platform-agnostic event loop driver
 */

#include "event_private.h"
#include <limits.h>
#include <signal.h>

/* ───────────────── Backend selection ───────────────── */

#ifdef X_HAS_KQUEUE
#define X_BACKEND (&g_kqueue_backend)
#elif defined(X_HAS_EPOLL)
#define X_BACKEND (&g_epoll_backend)
#elif defined(_WIN32)
#define X_BACKEND (&g_wsapoll_backend)
#else
#define X_BACKEND (&g_poll_backend)
#endif

/* ───────────────── Thread-local event loop ───────────────── */

#ifdef _WIN32
static __declspec(thread) xEventLoop tl_loop;
#else
static __thread xEventLoop tl_loop;
#endif

xEventLoop xEventLoopEnter(xEventLoop loop) {
  xEventLoop          old = tl_loop;
  struct xEventLoop_ *l   = (struct xEventLoop_ *)loop;
  if (l) l->prev = (struct xEventLoop_ *)old;
  tl_loop = loop;
  return old;
}

xEventLoop xEventLoopLeave(void) {
  struct xEventLoop_ *cur = (struct xEventLoop_ *)tl_loop;
  xEventLoop          old = tl_loop;
  tl_loop                 = cur ? (xEventLoop)cur->prev : NULL;
  return old;
}
xEventLoop xEventLoopCurrent(void) {
  return tl_loop;
}

/* ───────────────── Create / Destroy ───────────────── */

xEventLoop xEventLoopCreate(void) {
  return xEventLoopCreateWithGroup(NULL);
}

xEventLoop xEventLoopCreateWithGroup(xTaskGroup group) {
  const struct xEventBackend_ *be   = X_BACKEND;
  struct xEventLoop_          *loop = (struct xEventLoop_ *)calloc(1, be->size);
  if (!loop) return NULL;

  loop->backend    = be;
  loop->task_group = group;

  if (be->init(loop) != xErrno_Ok) {
    free(loop);
    return NULL;
  }
  return (xEventLoop)loop;
}

void xEventLoopDestroy(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;
  loop->backend->destroy(loop);
  free(loop);
}

/* ───────────────── Run modes ───────────────── */

#define X_RUN_DEFAULT (-1)
#define X_RUN_ONCE    (-2)
#define X_RUN_NOWAIT  (-3)

static int loop_next_timeout(struct xEventLoop_ *loop, int mode, int can_sleep) {
  if (mode == X_RUN_NOWAIT) return 0;
  if (mode == X_RUN_ONCE && !can_sleep) return 0;

  struct xTimer_ *top = (struct xTimer_ *)xHeapPeek(loop->timer_heap);
  if (!top) return (mode == X_RUN_ONCE) ? 0 : -1;

  int64_t wait = (int64_t)(top->deadline - loop->time);
  if (wait < 0) wait = 0;
  return (wait > INT_MAX) ? INT_MAX : (int)wait;
}

static int loop_poll_and_dispatch(struct xEventLoop_ *loop, int timeout_ms) {
  struct xPollEvent_ events[X_EVENT_IO_BATCH_MAX];
  int                n = loop->backend->poll(loop, events, X_EVENT_IO_BATCH_MAX, timeout_ms);
  if (n < 0) return 0;

  int dispatched = 0;
  for (int i = 0; i < n; i++) {
    switch (events[i].type) {
    case X_POLL_WAKE:
      break;
    case X_POLL_SIGNAL: {
      int signo = events[i].fd;
      if (signo > 0 && signo < X_SIGNAL_MAX && loop->signal_watches[signo].fn) {
        loop->signal_watches[signo].fn(signo, loop->signal_watches[signo].arg);
        dispatched++;
      }
      break;
    }
    case X_POLL_FD: {
      struct xEventSource_ *src = events[i].user_data;
      if (src && !src->deleted) {
        src->fn(src->fd, events[i].mask, src->arg);
        dispatched++;
      }
      break;
    }
    }
  }
  return dispatched;
}

/* ───────────────── Backend fd / timeout ───────────────── */

int xEventLoopFd(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return -1;
  return loop->backend->fd(loop);
}

int xEventLoopNextTimeout(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return -1;

  loop_update_time(loop);
  struct xTimer_ *top = (struct xTimer_ *)xHeapPeek(loop->timer_heap);
  if (!top) return -1;

  int64_t wait = (int64_t)(top->deadline - loop->time);
  if (wait < 0) wait = 0;
  return (wait > INT_MAX) ? INT_MAX : (int)wait;
}

/* ───────────────── xEventLoopRun ───────────────── */

int xEventLoopRun(xEventLoop loop_, int mode) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return 0;

  xEventLoopEnter(loop_);

  int alive = loop_alive(loop);
  if (!alive) {
    loop_update_time(loop);
  }

  if (mode == X_RUN_DEFAULT && alive && loop->stopped == 0) {
    loop_update_time(loop);
    loop_run_timers(loop);
  }

  while (alive && !loop->stopped) {
    int can_sleep = (loop->done_head == NULL);

    loop_run_done(loop, EVENT_DONE_BATCH_MAX);
    loop_poll_and_dispatch(loop, loop_next_timeout(loop, mode, can_sleep));
    loop_run_done(loop, EVENT_DONE_BATCH_MAX);

    loop_update_time(loop);
    loop_run_timers(loop);
    loop_sweep(loop);

    alive = loop_alive(loop);
    if (mode == X_RUN_ONCE || mode == X_RUN_NOWAIT) break;
  }

  if (loop->stopped != 0) {
    loop->stopped = 0;
  }

  xEventLoopLeave();
  return alive;
}

/* ───────────────── Stop / Wake ───────────────── */

void xEventLoopStop(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return;
  loop->stopped = 1;
  loop->backend->wake(loop);
}

xErrno xEventLoopWake(xEventLoop loop_) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)loop_;
  if (!loop) return xErrno_InvalidArg;
  loop->backend->wake(loop);
  return xErrno_Ok;
}

/* ───────────────── fd registration ───────────────── */

xEventSource xEventAdd(int fd, xEventMask mask, xEventFunc fn, void *arg) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)xEventLoopCurrent();
  if (!loop || !fn) return NULL;

  struct xEventSource_ *src = source_array_add(&loop->sources, fd, mask, fn, arg);
  if (!src) return NULL;

  if (loop->backend->add(loop, src) != xErrno_Ok) {
    source_array_remove(&loop->sources, src);
    return NULL;
  }

  loop->active_handles++;
  return (xEventSource)src;
}

xErrno xEventMod(xEventSource src_, xEventMask mask) {
  struct xEventLoop_   *loop = (struct xEventLoop_ *)xEventLoopCurrent();
  struct xEventSource_ *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;
  return loop->backend->mod(loop, src, mask);
}

xErrno xEventDel(xEventSource src_) {
  struct xEventLoop_   *loop = (struct xEventLoop_ *)xEventLoopCurrent();
  struct xEventSource_ *src  = (struct xEventSource_ *)src_;
  if (!loop || !src) return xErrno_InvalidArg;
  loop->active_handles--;
  return loop->backend->del(loop, src);
}

/* ───────────────── Signal watch ───────────────── */

xErrno xSignal(int signo, xSignalFunc fn, void *arg) {
  struct xEventLoop_ *loop = (struct xEventLoop_ *)xEventLoopCurrent();
  if (!loop || signo < 1 || signo >= X_SIGNAL_MAX) return xErrno_InvalidArg;
  if (signo == SIGKILL || signo == SIGSTOP) return xErrno_InvalidArg;

  int is_new = (loop->signal_watches[signo].fn == NULL);

  if (fn) {
    loop->signal_watches[signo].fn  = fn;
    loop->signal_watches[signo].arg = arg;
    if (is_new) {
      xErrno rc = loop->backend->signal(loop, signo, fn);
      if (rc != xErrno_Ok) {
        loop->signal_watches[signo].fn  = NULL;
        loop->signal_watches[signo].arg = NULL;
        return rc;
      }
    }
  } else {
    if (!is_new) {
      loop->backend->signal(loop, signo, NULL);
      loop->signal_watches[signo].fn  = NULL;
      loop->signal_watches[signo].arg = NULL;
    }
  }
  return xErrno_Ok;
}
