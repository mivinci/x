/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_poll.c - poll(2)-based event loop (POSIX fallback)
 *
 * Used when neither kqueue nor epoll is available.
 * poll(2) is level-triggered by nature; we emulate edge-triggered
 * semantics by disabling events after each notification and requiring
 * the caller to re-arm via xEventMod().
 */

#if !defined(X_HAS_KQUEUE) && !defined(X_HAS_EPOLL)

#include "event_private.h"
#include "event_signal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static short mask_to_poll(xEventMask mask) {
  short ev = 0;
  if (mask & xEvent_Read) ev |= POLLIN;
  if (mask & xEvent_Write) ev |= POLLOUT;
  return ev;
}

/* ───────────────────── Poll-specific loop data ───────────────────── */

struct xEventLoopPoll_ {
  struct xEventLoop_ base;

  /* Parallel arrays: pollfds[i] corresponds to sources.items[i],
   * except pollfds[0] is always the wake pipe. */
  struct pollfd *pollfds;
  size_t         pfd_len;
  size_t         pfd_cap;

  /* Self-pipe trick for signal delivery */
  int signal_pipe_r[X_SIGNAL_MAX]; /* read end, -1 = unused */
  int signal_pipe_w[X_SIGNAL_MAX]; /* write end, -1 = unused */
};

static int pfd_grow(struct xEventLoopPoll_ *loop) {
  size_t         newcap = loop->pfd_cap ? loop->pfd_cap * 2 : 16;
  struct pollfd *tmp    = (struct pollfd *)realloc(loop->pollfds, newcap * sizeof(struct pollfd));
  if (!tmp) return -1;
  loop->pollfds = tmp;
  loop->pfd_cap = newcap;
  return 0;
}

/* Rebuild pollfds array from sources list + wake pipe + signal pipes */
static void pfd_rebuild(struct xEventLoopPoll_ *loop) {
  /* Count active signal pipes */
  size_t nsig = 0;
  for (int i = 1; i < X_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0) nsig++;
  }

  size_t needed = 1 + loop->base.sources.len + nsig; /* wake + sources + signals */
  while (loop->pfd_cap < needed)
    pfd_grow(loop);

  /* Slot 0: wake pipe */
  loop->pollfds[0].fd      = loop->base.wake_rfd;
  loop->pollfds[0].events  = POLLIN;
  loop->pollfds[0].revents = 0;

  for (size_t i = 0; i < loop->base.sources.len; i++) {
    struct xEventSource_ *src    = loop->base.sources.items[i];
    loop->pollfds[1 + i].fd      = src->fd;
    loop->pollfds[1 + i].events  = mask_to_poll(src->mask);
    loop->pollfds[1 + i].revents = 0;
  }

  /* Append signal pipe read ends */
  size_t idx = 1 + loop->base.sources.len;
  for (int i = 1; i < X_SIGNAL_MAX; i++) {
    if (loop->signal_pipe_r[i] >= 0) {
      loop->pollfds[idx].fd      = loop->signal_pipe_r[i];
      loop->pollfds[idx].events  = POLLIN;
      loop->pollfds[idx].revents = 0;
      idx++;
    }
  }
  loop->pfd_len = needed;
}

/* ───────────────────── Backend vtable implementations ───────────────────── */

static xErrno poll_init(struct xEventLoop_ *loop) {
  struct xEventLoopPoll_ *pl = (struct xEventLoopPoll_ *)loop;

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
    pl->signal_pipe_r[i] = -1;
    pl->signal_pipe_w[i] = -1;
  }

  if (loop_init_wake(loop) != 0) goto fail;
  if (set_nonblock(loop->wake_rfd) != 0) goto fail;
  if (set_nonblock(loop->wake_wfd) != 0) goto fail;

  return xErrno_Ok;

fail:
  loop_close_wake(loop);
  source_array_free(&loop->sources);
  if (loop->timer_heap) {
    xHeapDestroy(loop->timer_heap);
  }
  return xErrno_SysError;
}

static void poll_destroy(struct xEventLoop_ *loop) {
  struct xEventLoopPoll_ *pl = (struct xEventLoopPoll_ *)loop;

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

  /* Close signal pipes */
  for (int i = 0; i < X_SIGNAL_MAX; i++) {
    if (pl->signal_pipe_r[i] >= 0) close(pl->signal_pipe_r[i]);
    if (pl->signal_pipe_w[i] >= 0) close(pl->signal_pipe_w[i]);
  }

  loop_close_wake(loop);
  source_array_free(&loop->sources);
  free(pl->pollfds);
}

static int poll_poll(struct xEventLoop_ *loop, struct xPollEvent_ *events, int max_events, int timeout_ms) {
  struct xEventLoopPoll_ *pl = (struct xEventLoopPoll_ *)loop;

  pfd_rebuild(pl);

  int n = poll(pl->pollfds, (nfds_t)pl->pfd_len, timeout_ms);
  if (n < 0) n = 0;

  int count = 0;

  /* Check wake pipe (slot 0) */
  if (pl->pollfds[0].revents & POLLIN) {
    loop_drain_wake(loop);
    loop_clear_wake_pending(loop);
    loop_run_done(loop, EVENT_DONE_BATCH_MAX);
    events[count].type      = X_POLL_WAKE;
    events[count].fd        = 0;
    events[count].mask      = 0;
    events[count].user_data = NULL;
    count++;
  }

  /* Check sources (slots 1..sources.len) */
  for (size_t i = 0; i < loop->sources.len && count < max_events; i++) {
    struct pollfd *pfd = &pl->pollfds[1 + i];
    if (pfd->revents == 0) continue;

    struct xEventSource_ *src = loop->sources.items[i];
    if (src->deleted) continue;

    xEventMask ready = 0;
    if (pfd->revents & POLLIN)  ready |= xEvent_Read;
    if (pfd->revents & POLLOUT) ready |= xEvent_Write;

    if (ready) {
      if (!src->level_triggered) {
        /* Edge-triggered: disable mask to prevent level-triggered re-fire */
        xEventMask orig_mask = src->mask;
        src->mask            = 0;

        events[count].type      = X_POLL_FD;
        events[count].fd        = src->fd;
        events[count].mask      = ready;
        events[count].user_data = src;
        count++;

        /* Re-arm the source if the fd was fully drained. */
        if (!src->deleted && src->mask == 0) {
          xEventMask restore = 0;
          if (orig_mask & xEvent_Read) {
            int avail = 0;
            if (ioctl(src->fd, FIONREAD, &avail) == 0 && avail == 0) {
              restore |= (orig_mask & xEvent_Read);
            }
          }
          if (orig_mask & xEvent_Write) {
            restore |= (orig_mask & xEvent_Write);
          }
          src->mask = restore;
        }
      } else {
        /* Level-triggered: poll naturally reports ready fds every time.
         * No mask manipulation needed — just report the event. */
        events[count].type      = X_POLL_FD;
        events[count].fd        = src->fd;
        events[count].mask      = ready;
        events[count].user_data = src;
        count++;
      }
    }
  }

  /* Check signal pipes */
  size_t sig_base = 1 + loop->sources.len;
  size_t sig_idx  = 0;
  for (int s = 1; s < X_SIGNAL_MAX && count < max_events; s++) {
    if (pl->signal_pipe_r[s] < 0) continue;
    struct pollfd *pfd = &pl->pollfds[sig_base + sig_idx];
    sig_idx++;
    if (!(pfd->revents & POLLIN)) continue;

    /* Drain the pipe */
    char buf[64];
    while (read(pl->signal_pipe_r[s], buf, sizeof(buf)) > 0)
      ;

    events[count].type      = X_POLL_SIGNAL;
    events[count].fd        = s;
    events[count].mask      = 0;
    events[count].user_data = NULL;
    count++;
  }

  return count;
}

static void poll_wake(struct xEventLoop_ *loop) {
  char    c = 1;
  ssize_t r;
  do {
    r = write(loop->wake_wfd, &c, 1);
  } while (r < 0 && errno == EINTR);
}

static int poll_fd(struct xEventLoop_ *loop) {
  (void)loop;
  return -1;
}

static xErrno poll_add(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  if (set_nonblock(src->fd) != 0) return xErrno_SysError;
  return xErrno_Ok;
}

static xErrno poll_mod(struct xEventLoop_ *loop, struct xEventSource_ *src, xEventMask mask) {
  (void)loop;
  src->mask = mask;
  return xErrno_Ok;
}

static xErrno poll_del(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  (void)loop;
  source_array_remove(&loop->sources, src);
  return xErrno_Ok;
}

static xErrno poll_signal(struct xEventLoop_ *loop, int signo, xSignalFunc fn) {
  struct xEventLoopPoll_ *pl = (struct xEventLoopPoll_ *)loop;

  if (fn) {
    if (pl->signal_pipe_r[signo] >= 0) {
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

    pl->signal_pipe_r[signo] = fds[0];
    pl->signal_pipe_w[signo] = fds[1];
    g_signal_pipe_w[signo]   = fds[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(signo, &sa, NULL) < 0) {
      close(fds[0]);
      close(fds[1]);
      pl->signal_pipe_r[signo] = -1;
      pl->signal_pipe_w[signo] = -1;
      g_signal_pipe_w[signo]   = -1;
      return xErrno_SysError;
    }
  } else {
    if (pl->signal_pipe_r[signo] < 0) return xErrno_Ok; /* nothing to cancel */

    signal(signo, SIG_DFL);
    g_signal_pipe_w[signo] = -1;

    close(pl->signal_pipe_r[signo]);
    close(pl->signal_pipe_w[signo]);
    pl->signal_pipe_r[signo] = -1;
    pl->signal_pipe_w[signo] = -1;
  }

  return xErrno_Ok;
}

/* ───────────────────── Backend vtable ───────────────────── */

const struct xEventBackend_ g_poll_backend = {
  .size       = sizeof(struct xEventLoopPoll_),
  .init       = poll_init,
  .destroy    = poll_destroy,
  .poll       = poll_poll,
  .wake       = poll_wake,
  .fd         = poll_fd,
  .add        = poll_add,
  .mod        = poll_mod,
  .del        = poll_del,
  .signal = poll_signal,
};

#endif /* !X_HAS_KQUEUE && !X_HAS_EPOLL */
