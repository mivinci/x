/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_epoll.c - epoll-based event loop backend (edge-triggered)
 *
 * Why self-pipe instead of signalfd(2)?
 *
 * signalfd requires the target signal to be blocked (via sigprocmask /
 * pthread_sigmask) in *every* thread of the process.  pthread_sigmask only
 * affects the calling thread; any thread that has not blocked the signal
 * will receive it with the default disposition — typically process
 * termination.  In practice this is fragile: third-party libraries, test
 * frameworks (e.g. gtest death-tests), or thread-pools may spawn threads
 * that never call pthread_sigmask, so the signal races to an unblocked
 * thread and kills the process.
 *
 * The self-pipe trick avoids the problem entirely: we install a normal
 * signal handler via sigaction(2) which writes a byte into a pipe.  The
 * handler runs on whichever thread receives the signal (safe — write(2) to
 * a pipe is async-signal-safe), and the event loop picks it up through the
 * pipe's read end registered with epoll.  No thread-wide signal mask
 * manipulation is needed.
 */

#ifdef X_HAS_EPOLL

#include "event_private.h"
#include "event_signal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t mask_to_epoll(xEventMask mask) {
  uint32_t ev = EPOLLET; /* always edge-triggered */
  if (mask & xEvent_Read) ev |= EPOLLIN;
  if (mask & xEvent_Write) ev |= EPOLLOUT;
  return ev;
}

/* ───────────────────── Epoll-specific loop data ───────────────────── */

struct xEventLoopEpoll_ {
  struct xEventLoop_ base;
  int                epfd;
  /* Self-pipe trick for signal delivery */
  int signal_pipe_r[X_SIGNAL_MAX]; /* read end, -1 = unused */
  int signal_pipe_w[X_SIGNAL_MAX]; /* write end, -1 = unused */
};

/* Check whether an epoll fd belongs to a signal pipe read end.
 * Returns the signal number, or 0 if not found. */
static int find_signal_by_fd(struct xEventLoopEpoll_ *loop, int fd) {
  for (int i = 1; i < X_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0 && loop->signal_pipe_r[i] == fd) return i;
  }
  return 0;
}

/* ───────────────────── Backend vtable implementations ───────────────────── */

static xErrno ep_init(struct xEventLoop_ *loop) {
  struct xEventLoopEpoll_ *el = (struct xEventLoopEpoll_ *)loop;

  el->epfd            = -1;
  loop->wake_rfd      = -1;
  loop->wake_wfd      = -1;
  loop->task_group    = NULL;
  source_array_init(&loop->sources);
  loop->done_head     = NULL;
  loop->done_tail     = NULL;
  loop->work_freelist = NULL;
  xAtomicStore(&loop->inflight, 0, xAtomicRelaxed);
  xAtomicStore(&loop->wake_pending, 0, xAtomicRelaxed);

  loop->timer_heap = xHeapCreate(timer_cmp, timer_set_idx, 0);
  if (!loop->timer_heap) goto fail;

  for (int i = 0; i < X_SIGNAL_MAX; i++) {
    el->signal_pipe_r[i] = -1;
    el->signal_pipe_w[i] = -1;
  }

  el->epfd = epoll_create1(EPOLL_CLOEXEC);
  if (el->epfd < 0) goto fail;

  /* Use eventfd for lightweight wake (single fd, no pipe). */
  loop->wake_rfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (loop->wake_rfd < 0) goto fail;

  /* Register eventfd with epoll (edge-triggered) */
  struct epoll_event ev;
  ev.events  = EPOLLIN | EPOLLET;
  ev.data.fd = loop->wake_rfd;
  if (epoll_ctl(el->epfd, EPOLL_CTL_ADD, loop->wake_rfd, &ev) != 0) goto fail;

  return xErrno_Ok;

fail:
  if (el->epfd >= 0) close(el->epfd);
  if (loop->wake_rfd >= 0) close(loop->wake_rfd);
  source_array_free(&loop->sources);
  if (loop->timer_heap) {
    xHeapDestroy(loop->timer_heap);
  }
  return xErrno_SysError;
}

static void ep_destroy(struct xEventLoop_ *loop) {
  struct xEventLoopEpoll_ *el = (struct xEventLoopEpoll_ *)loop;

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

  /* Close any open signal pipes */
  for (int i = 0; i < X_SIGNAL_MAX; i++) {
    if (el->signal_pipe_r[i] >= 0) {
      epoll_ctl(el->epfd, EPOLL_CTL_DEL, el->signal_pipe_r[i], NULL);
      close(el->signal_pipe_r[i]);
    }
    if (el->signal_pipe_w[i] >= 0) close(el->signal_pipe_w[i]);
  }

  if (loop->wake_rfd >= 0) close(loop->wake_rfd);
  close(el->epfd);
  source_array_free(&loop->sources);
}

static int ep_poll(struct xEventLoop_ *loop, struct xPollEvent_ *events, int max_events, int timeout_ms) {
  struct xEventLoopEpoll_ *el = (struct xEventLoopEpoll_ *)loop;

  struct epoll_event epevents[X_EVENT_IO_BATCH_MAX];
  int                n = epoll_wait(el->epfd, epevents, X_EVENT_IO_BATCH_MAX, timeout_ms);
  if (n < 0) n = 0;

  int count = 0;
  for (int i = 0; i < n && count < max_events; i++) {
    int efd = epevents[i].data.fd;

    /* Wake eventfd — registered with data.fd */
    if (efd == loop->wake_rfd) {
      /* Drain the eventfd counter */
      uint64_t val;
      (void)read(loop->wake_rfd, &val, sizeof(val));
      loop_clear_wake_pending(loop);
      loop_run_done(loop, EVENT_DONE_BATCH_MAX);
      events[count].type      = X_POLL_WAKE;
      events[count].fd        = 0;
      events[count].mask      = 0;
      events[count].user_data = NULL;
      count++;
      continue;
    }

    /* Check if this is a signal pipe event — registered with data.fd */
    int signo = find_signal_by_fd(el, efd);
    if (signo > 0) {
      /* Drain the signal pipe */
      char buf[64];
      while (read(efd, buf, sizeof(buf)) > 0)
        ;

      events[count].type      = X_POLL_SIGNAL;
      events[count].fd        = signo;
      events[count].mask      = 0;
      events[count].user_data = NULL;
      count++;
      continue;
    }

    /* User event source — registered with data.ptr */
    struct xEventSource_ *src = (struct xEventSource_ *)epevents[i].data.ptr;
    if (!src || src->deleted) continue;

    xEventMask mask = 0;
    if (epevents[i].events & EPOLLIN) mask |= xEvent_Read;
    if (epevents[i].events & EPOLLOUT) mask |= xEvent_Write;

    events[count].type      = X_POLL_FD;
    events[count].fd        = src->fd;
    events[count].mask      = mask;
    events[count].user_data = src;
    count++;
  }

  return count;
}

static void ep_wake(struct xEventLoop_ *loop) {
  uint64_t val = 1;
  ssize_t  r;
  do {
    r = write(loop->wake_rfd, &val, sizeof(val));
  } while (r < 0 && errno == EINTR);
}

static int ep_fd(struct xEventLoop_ *loop) {
  return ((struct xEventLoopEpoll_ *)loop)->epfd;
}

static xErrno ep_add(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  struct xEventLoopEpoll_ *el = (struct xEventLoopEpoll_ *)loop;

  if (set_nonblock(src->fd) != 0) return xErrno_SysError;

  struct epoll_event ev;
  ev.events   = mask_to_epoll(src->mask);
  ev.data.ptr = src;
  if (epoll_ctl(el->epfd, EPOLL_CTL_ADD, src->fd, &ev) != 0) return xErrno_SysError;

  return xErrno_Ok;
}

static xErrno ep_mod(struct xEventLoop_ *loop, struct xEventSource_ *src, xEventMask mask) {
  struct xEventLoopEpoll_ *el = (struct xEventLoopEpoll_ *)loop;

  struct epoll_event ev;
  ev.events   = mask_to_epoll(mask);
  ev.data.ptr = src;
  if (epoll_ctl(el->epfd, EPOLL_CTL_MOD, src->fd, &ev) != 0) return xErrno_SysError;

  src->mask = mask;
  return xErrno_Ok;
}

static xErrno ep_del(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  struct xEventLoopEpoll_ *el = (struct xEventLoopEpoll_ *)loop;

  epoll_ctl(el->epfd, EPOLL_CTL_DEL, src->fd, NULL);
  source_array_remove(&loop->sources, src);
  return xErrno_Ok;
}

static xErrno ep_signal(struct xEventLoop_ *loop, int signo, xSignalFunc fn) {
  struct xEventLoopEpoll_ *el = (struct xEventLoopEpoll_ *)loop;

  if (fn) {
    if (el->signal_pipe_r[signo] >= 0) {
      /* Already have a pipe — reuse (callback is set by the caller) */
      return xErrno_Ok;
    }

    int fds[2];
    if (pipe(fds) != 0) return xErrno_SysError;
    if (set_nonblock(fds[0]) != 0 || set_nonblock(fds[1]) != 0) {
      close(fds[0]);
      close(fds[1]);
      return xErrno_SysError;
    }

    /* Register the pipe read end with epoll (edge-triggered) */
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = fds[0];
    if (epoll_ctl(el->epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
      close(fds[0]);
      close(fds[1]);
      return xErrno_SysError;
    }

    el->signal_pipe_r[signo] = fds[0];
    el->signal_pipe_w[signo] = fds[1];
    g_signal_pipe_w[signo]   = fds[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(signo, &sa, NULL) < 0) {
      epoll_ctl(el->epfd, EPOLL_CTL_DEL, fds[0], NULL);
      close(fds[0]);
      close(fds[1]);
      el->signal_pipe_r[signo] = -1;
      el->signal_pipe_w[signo] = -1;
      g_signal_pipe_w[signo]   = -1;
      return xErrno_SysError;
    }
  } else {
    if (el->signal_pipe_r[signo] < 0) return xErrno_Ok; /* nothing to cancel */

    signal(signo, SIG_DFL);
    g_signal_pipe_w[signo] = -1;

    epoll_ctl(el->epfd, EPOLL_CTL_DEL, el->signal_pipe_r[signo], NULL);
    close(el->signal_pipe_r[signo]);
    close(el->signal_pipe_w[signo]);
    el->signal_pipe_r[signo] = -1;
    el->signal_pipe_w[signo] = -1;
  }

  return xErrno_Ok;
}

/* ───────────────────── Backend vtable ───────────────────── */

const struct xEventBackend_ g_epoll_backend = {
  .size       = sizeof(struct xEventLoopEpoll_),
  .init       = ep_init,
  .destroy    = ep_destroy,
  .poll       = ep_poll,
  .wake       = ep_wake,
  .fd         = ep_fd,
  .add        = ep_add,
  .mod        = ep_mod,
  .del        = ep_del,
  .signal = ep_signal,
};

#endif /* X_HAS_EPOLL */
