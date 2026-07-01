/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_kqueue.c - kqueue-based event loop backend (edge-triggered)
 */

#ifdef X_HAS_KQUEUE

#include "event_private.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/types.h>

#include <x/base/log.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Register or update filters for a source on the kqueue fd.
 * Uses EV_CLEAR for edge-triggered, omits it for level-triggered.
 */
static int kq_apply(int kqfd, struct xEventSource_ *src, xEventMask mask) {
  struct kevent changes[2];
  int           n     = 0;
  int           flags = EV_ADD | (src->level_triggered ? 0 : EV_CLEAR);

  if (mask & xEvent_Read) {
    EV_SET(&changes[n], src->fd, EVFILT_READ, flags, 0, 0, src);
    n++;
  } else {
    /* Remove read filter if it was previously set */
    EV_SET(&changes[n], src->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    n++;
  }

  if (mask & xEvent_Write) {
    EV_SET(&changes[n], src->fd, EVFILT_WRITE, flags, 0, 0, src);
    n++;
  } else {
    EV_SET(&changes[n], src->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    n++;
  }

  /* Ignore ENOENT errors from deleting filters that don't exist */
  for (int i = 0; i < n; i++) {
    if (kevent(kqfd, &changes[i], 1, NULL, 0, NULL) < 0 && errno != ENOENT) return -1;
  }
  return 0;
}

/* ───────────────────── Kqueue-specific loop data ───────────────────── */

/* Arbitrary ident for the EVFILT_USER wake event (not a real fd). */
#define KQ_WAKE_IDENT 0xBADC0FFEu

struct xEventLoopKqueue_ {
  struct xEventLoop_ base;
  int                kqfd;
};

/* ───────────────────── Signal helpers ───────────────────── */

__attribute__((unused)) static int signo_valid(int signo) {
  return signo > 0 && signo < X_SIGNAL_MAX && signo != SIGKILL && signo != SIGSTOP;
}

/* On macOS/BSD, kqueue's EVFILT_SIGNAL only fires if the signal is actually
 * delivered to the process.  Setting SIG_IGN prevents delivery entirely,
 * so the kqueue filter never triggers.  Use a no-op handler instead. */
static void signal_noop(int signo) {
  (void)signo;
}

/* ───────────────────── Backend vtable implementations ───────────────────── */

static xErrno kq_init(struct xEventLoop_ *loop) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  kl->kqfd = -1;

  /* base fields already zeroed by calloc in event_run.c */
  loop->wake_rfd   = -1;
  loop->wake_wfd   = -1;
  loop->task_group = NULL;
  source_array_init(&loop->sources);
  loop->done_head     = NULL;
  loop->done_tail     = NULL;
  loop->work_freelist = NULL;
  xAtomicStore(&loop->inflight, 0, xAtomicRelaxed);
  xAtomicStore(&loop->wake_pending, 0, xAtomicRelaxed);

  loop->timer_heap = xHeapCreate(timer_cmp, timer_set_idx, 0);
  if (!loop->timer_heap) goto fail;

  kl->kqfd = kqueue();
  if (kl->kqfd < 0) goto fail;

  /* Register EVFILT_USER for lightweight wake (no pipe needed). */
  struct kevent ev;
  EV_SET(&ev, KQ_WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  if (kevent(kl->kqfd, &ev, 1, NULL, 0, NULL) < 0) goto fail;

  return xErrno_Ok;

fail:
  if (kl->kqfd >= 0) close(kl->kqfd);
  source_array_free(&loop->sources);
  if (loop->timer_heap) {
    xHeapDestroy(loop->timer_heap);
  }
  return xErrno_SysError;
}

static void kq_destroy(struct xEventLoop_ *loop) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  /* Discard all pending timers without firing */
  while (xHeapSize(loop->timer_heap) > 0) {
    struct xTimer_ *t = (struct xTimer_ *)xHeapPop(loop->timer_heap);
    timer_free(loop, t);
  }
  timer_pool_destroy(loop);
  xHeapDestroy(loop->timer_heap);

  loop_wait_inflight(loop);
  loop_cleanup_done(loop);
  event_work_pool_destroy(loop);

  close(kl->kqfd);
  source_array_free(&loop->sources);
}

static int kq_poll(struct xEventLoop_ *loop, struct xPollEvent_ *events, int max_events,
                   int timeout_ms) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  struct kevent    kevents[X_EVENT_IO_BATCH_MAX];
  struct timespec  ts;
  struct timespec *tsp = NULL;

  if (timeout_ms >= 0) {
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
    tsp        = &ts;
  }

  XDEBUGL1("kq: poll timeout=%d", timeout_ms);

  int n = kevent(kl->kqfd, NULL, 0, kevents, X_EVENT_IO_BATCH_MAX, tsp);
  if (n < 0) n = 0; /* treat EINTR as no events */

  if (n > 0) XDEBUGL1("kq: got %d events", n);

  int count = 0;
  for (int i = 0; i < n && count < max_events; i++) {
    /* EVFILT_USER wake event */
    if (kevents[i].filter == EVFILT_USER && kevents[i].ident == KQ_WAKE_IDENT) {
      loop_clear_wake_pending(loop);
      loop_run_done(loop, EVENT_DONE_BATCH_MAX);
      events[count].type      = X_POLL_WAKE;
      events[count].fd        = 0;
      events[count].mask      = 0;
      events[count].user_data = NULL;
      count++;
      continue;
    }

    /* Signal event */
    if (kevents[i].filter == EVFILT_SIGNAL) {
      int signo               = (int)kevents[i].ident;
      events[count].type      = X_POLL_SIGNAL;
      events[count].fd        = signo;
      events[count].mask      = 0;
      events[count].user_data = NULL;
      count++;
      continue;
    }

    /* FD event */
    struct xEventSource_ *src = (struct xEventSource_ *)kevents[i].udata;
    if (!src || src->deleted) continue;

    xEventMask mask = 0;
    if (kevents[i].filter == EVFILT_READ) mask |= xEvent_Read;
    if (kevents[i].filter == EVFILT_WRITE) mask |= xEvent_Write;

    XDEBUGL1("kq: fd=%d %s%s src=%p", src->fd, (mask & xEvent_Read) ? "R" : "",
             (mask & xEvent_Write) ? "W" : "", (void *)src);

    events[count].type      = X_POLL_FD;
    events[count].fd        = src->fd;
    events[count].mask      = mask;
    events[count].user_data = src;
    count++;
  }

  return count;
}

static void kq_wake(struct xEventLoop_ *loop) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  struct kevent ev;
  EV_SET(&ev, KQ_WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  kevent(kl->kqfd, &ev, 1, NULL, 0, NULL);
}

static int kq_fd(struct xEventLoop_ *loop) {
  return ((struct xEventLoopKqueue_ *)loop)->kqfd;
}

static xErrno kq_add(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  XDEBUGL1("kq: add fd=%d mask=%x lt=%d", src->fd, src->mask, src->level_triggered);

  if (set_nonblock(src->fd) != 0) return xErrno_SysError;
  if (kq_apply(kl->kqfd, src, src->mask) != 0) return xErrno_SysError;

  return xErrno_Ok;
}

static xErrno kq_mod(struct xEventLoop_ *loop, struct xEventSource_ *src, xEventMask mask) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  if (kq_apply(kl->kqfd, src, mask) != 0) return xErrno_SysError;

  src->mask = mask;
  return xErrno_Ok;
}

static xErrno kq_del(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  /* Remove all filters */
  struct kevent changes[2];
  EV_SET(&changes[0], src->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&changes[1], src->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  /* Ignore ENOENT */
  kevent(kl->kqfd, &changes[0], 1, NULL, 0, NULL);
  kevent(kl->kqfd, &changes[1], 1, NULL, 0, NULL);

  source_array_remove(&loop->sources, src);
  return xErrno_Ok;
}

static xErrno kq_signal(struct xEventLoop_ *loop, int signo, xSignalFunc fn) {
  struct xEventLoopKqueue_ *kl = (struct xEventLoopKqueue_ *)loop;

  if (fn) {
    signal(signo, signal_noop);

    struct kevent ev;
    EV_SET(&ev, signo, EVFILT_SIGNAL, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kl->kqfd, &ev, 1, NULL, 0, NULL) < 0) {
      signal(signo, SIG_DFL);
      return xErrno_SysError;
    }
  } else {
    struct kevent ev;
    EV_SET(&ev, signo, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
    kevent(kl->kqfd, &ev, 1, NULL, 0, NULL); /* ignore ENOENT */

    signal(signo, SIG_DFL);
  }

  return xErrno_Ok;
}

/* ───────────────────── Backend vtable ───────────────────── */

const struct xEventBackend_ g_kqueue_backend = {
  .size    = sizeof(struct xEventLoopKqueue_),
  .init    = kq_init,
  .destroy = kq_destroy,
  .poll    = kq_poll,
  .wake    = kq_wake,
  .fd      = kq_fd,
  .add     = kq_add,
  .mod     = kq_mod,
  .del     = kq_del,
  .signal  = kq_signal,
};

#endif /* X_HAS_KQUEUE */
