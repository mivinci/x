/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_wsapoll.c - WSAPoll-based event loop (Windows)
 *
 * Uses WSAPoll() for I/O multiplexing on Windows. API semantics are
 * identical to the POSIX poll() backend: edge-triggered emulation via
 * disabling events after each notification, requiring the caller to
 * re-arm via xEventMod().
 *
 * Wake mechanism: loopback socket pair (connect-to-localhost) replaces
 * the POSIX pipe(). Only sockets can be polled with WSAPoll; the
 * CreateEvent HANDLE in event_private.h is left unused by this backend.
 */

#ifdef _WIN32

#include "event_private.h"

#define WIN32_LEAN_AND_MEAN
#include <mswsock.h>

#include <winsock2.h>
#include <ws2tcpip.h>

/* ───────────────────── WSAStartup ───────────────────── */

static INIT_ONCE wsa_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wsa_init_fn(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  WSADATA d;
  WSAStartup(MAKEWORD(2, 2), &d);
  return TRUE;
}

static void wsa_ensure_init(void) {
  InitOnceExecuteOnce(&wsa_init_once, wsa_init_fn, NULL, NULL);
}

/* ───────────────────── Helpers ───────────────────── */

static int set_nonblock(SOCKET fd) {
  u_long mode = 1;
  return ioctlsocket(fd, FIONBIO, &mode);
}

static short mask_to_poll(xEventMask mask) {
  short ev = 0;
  if (mask & xEvent_Read) ev |= POLLIN;
  if (mask & xEvent_Write) ev |= POLLOUT;
  return ev;
}

/*
 * Create a loopback socket pair for the wake pipe.
 *
 * WSAPoll can only poll sockets, so we create a TCP connection to
 * localhost and use the two ends as read/write fds — same as a pipe().
 */
static int wake_socket_pair(SOCKET *rfd, SOCKET *wfd) {
  SOCKET listener = INVALID_SOCKET;
  SOCKET conn     = INVALID_SOCKET;
  SOCKET acceptor = INVALID_SOCKET;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) goto fail;

  /* Bind to loopback on an ephemeral port */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;

  if (listen(listener, 1) != 0) goto fail;

  /* Get the assigned port */
  int addrlen = sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) goto fail;

  /* Connect to self */
  conn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (conn == INVALID_SOCKET) goto fail;

  if (connect(conn, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;

  acceptor = accept(listener, NULL, NULL);
  if (acceptor == INVALID_SOCKET) goto fail;

  /* Set both ends non-blocking */
  if (set_nonblock(acceptor) != 0 || set_nonblock(conn) != 0) goto fail;

  closesocket(listener);

  *rfd = acceptor; /* read end */
  *wfd = conn;     /* write end */
  return 0;

fail:
  if (listener != INVALID_SOCKET) closesocket(listener);
  if (conn != INVALID_SOCKET) closesocket(conn);
  if (acceptor != INVALID_SOCKET) closesocket(acceptor);
  return -1;
}

/* ───────────────────── WSAPoll-specific loop data ───────────────────── */

struct xEventLoopWSAPoll_ {
  struct xEventLoop_ base;

  /* Loopback socket pair for wake (replaces pipe on POSIX) */
  SOCKET wake_rfd;
  SOCKET wake_wfd;

  /* Parallel arrays: pollfds[i] corresponds to sources.items[i],
   * except pollfds[0] is always the wake socket. */
  WSAPOLLFD *pollfds;
  size_t     pfd_len;
  size_t     pfd_cap;
};

static int pfd_grow(struct xEventLoopWSAPoll_ *loop) {
  size_t     newcap = loop->pfd_cap ? loop->pfd_cap * 2 : 16;
  WSAPOLLFD *tmp    = (WSAPOLLFD *)realloc(loop->pollfds, newcap * sizeof(WSAPOLLFD));
  if (!tmp) return -1;
  loop->pollfds = tmp;
  loop->pfd_cap = newcap;
  return 0;
}

/* Rebuild pollfds array from sources list + wake socket */
static void pfd_rebuild(struct xEventLoopWSAPoll_ *loop) {
  size_t needed = 1 + loop->base.sources.len; /* wake + sources */
  while (loop->pfd_cap < needed)
    pfd_grow(loop);

  /* Slot 0: wake socket */
  loop->pollfds[0].fd      = (SOCKET)loop->wake_rfd;
  loop->pollfds[0].events  = POLLIN;
  loop->pollfds[0].revents = 0;

  for (size_t i = 0; i < loop->base.sources.len; i++) {
    struct xEventSource_ *src    = loop->base.sources.items[i];
    loop->pollfds[1 + i].fd      = (SOCKET)src->fd;
    loop->pollfds[1 + i].events  = mask_to_poll(src->mask);
    loop->pollfds[1 + i].revents = 0;
  }

  loop->pfd_len = needed;
}

/* ───────────────────── Backend vtable implementations ───────────────────── */

static xErrno wsapoll_init(struct xEventLoop_ *loop) {
  struct xEventLoopWSAPoll_ *wl = (struct xEventLoopWSAPoll_ *)loop;

  wsa_ensure_init();

  loop->wake_event = NULL; /* not used by WSAPoll backend */
  loop->task_group = NULL;
  source_array_init(&loop->sources);
  loop->done_head     = NULL;
  loop->done_tail     = NULL;
  loop->work_freelist = NULL;
  xAtomicStore(&loop->inflight, 0, xAtomicRelaxed);
  xAtomicStore(&loop->wake_pending, 0, xAtomicRelaxed);

  loop->timer_heap = xHeapCreate(timer_cmp, timer_set_idx, 0);
  if (!loop->timer_heap) goto fail;

  if (wake_socket_pair(&wl->wake_rfd, &wl->wake_wfd) != 0) goto fail;

  return xErrno_Ok;

fail:
  if (wl->wake_rfd != INVALID_SOCKET) closesocket(wl->wake_rfd);
  if (wl->wake_wfd != INVALID_SOCKET) closesocket(wl->wake_wfd);
  source_array_free(&loop->sources);
  if (loop->timer_heap) {
    xHeapDestroy(loop->timer_heap);
  }
  return xErrno_SysError;
}

static void wsapoll_destroy(struct xEventLoop_ *loop) {
  struct xEventLoopWSAPoll_ *wl = (struct xEventLoopWSAPoll_ *)loop;

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

  closesocket(wl->wake_rfd);
  closesocket(wl->wake_wfd);

  source_array_free(&loop->sources);
  free(wl->pollfds);
}

static int wsapoll_poll(struct xEventLoop_ *loop, struct xPollEvent_ *events, int max_events,
                        int timeout_ms) {
  struct xEventLoopWSAPoll_ *wl = (struct xEventLoopWSAPoll_ *)loop;

  pfd_rebuild(wl);

  int n = WSAPoll(wl->pollfds, (ULONG)wl->pfd_len, timeout_ms);
  if (n < 0) n = 0;

  int count = 0;

  /* Check wake socket (slot 0) */
  if (wl->pollfds[0].revents & POLLIN) {
    /* Drain the wake socket */
    char buf[64];
    while (recv(wl->wake_rfd, buf, sizeof(buf), 0) > 0)
      ;
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
    WSAPOLLFD *pfd = &wl->pollfds[1 + i];
    if (pfd->revents == 0) continue;

    struct xEventSource_ *src = loop->sources.items[i];
    if (src->deleted) continue;

    xEventMask ready = 0;
    if (pfd->revents & POLLIN) ready |= xEvent_Read;
    if (pfd->revents & POLLOUT) ready |= xEvent_Write;
    /* POLLERR/POLLHUP map to read so the user gets notified */
    if (pfd->revents & (POLLERR | POLLHUP)) ready |= xEvent_Read;

    if (ready) {
      xEventMask orig_mask = src->mask;
      src->mask            = 0; /* edge-triggered: disable to prevent level-triggered re-fire */

      events[count].type      = X_POLL_FD;
      events[count].fd        = src->fd;
      events[count].mask      = ready;
      events[count].user_data = src;
      count++;

      /* Re-arm the source if the fd was fully drained.
       * This emulates EPOLLET's "empty → non-empty" edge semantics:
       * after the callback drains the buffer, restoring the mask
       * allows the next data arrival to trigger a new edge. */
      if (!src->deleted && src->mask == 0) {
        xEventMask restore = 0;
        if (orig_mask & xEvent_Read) {
          /* Peek to check if the read buffer is empty */
          char buf;
          int  r = recv((SOCKET)src->fd, &buf, 1, MSG_PEEK);
          if (r <= 0) {
            /* Buffer is empty (EWOULDBLOCK) or connection closed — re-arm */
            restore |= (orig_mask & xEvent_Read);
          }
          /* r > 0: data still available, keep read disabled (no new edge) */
        }
        if (orig_mask & xEvent_Write) {
          /* Re-arm write: if the write buffer was full (blocking send),
           * the fire indicated it became writable again. Restoring allows
           * detecting the next "full → writable" transition. */
          restore |= (orig_mask & xEvent_Write);
        }
        src->mask = restore;
      }
    }
  }

  return count;
}

static void wsapoll_wake(struct xEventLoop_ *loop) {
  struct xEventLoopWSAPoll_ *wl = (struct xEventLoopWSAPoll_ *)loop;

  char c = 1;
  send(wl->wake_wfd, &c, 1, 0);
}

static int wsapoll_fd(struct xEventLoop_ *loop) {
  (void)loop;
  return -1;
}

static xErrno wsapoll_add(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  (void)loop;
  if (set_nonblock((SOCKET)src->fd) != 0) return xErrno_SysError;
  return xErrno_Ok;
}

static xErrno wsapoll_mod(struct xEventLoop_ *loop, struct xEventSource_ *src, xEventMask mask) {
  (void)loop;
  src->mask = mask;
  return xErrno_Ok;
}

static xErrno wsapoll_del(struct xEventLoop_ *loop, struct xEventSource_ *src) {
  (void)loop;
  source_array_remove(&loop->sources, src);
  return xErrno_Ok;
}

static xErrno wsapoll_signal(struct xEventLoop_ *loop, int signo, xSignalFunc fn) {
  (void)loop;
  (void)signo;
  (void)fn;
  /* POSIX signals are not available on Windows. */
  return xErrno_InvalidArg;
}

/* ───────────────────── Backend vtable ───────────────────── */

const struct xEventBackend_ g_wsapoll_backend = {
  .size    = sizeof(struct xEventLoopWSAPoll_),
  .init    = wsapoll_init,
  .destroy = wsapoll_destroy,
  .poll    = wsapoll_poll,
  .wake    = wsapoll_wake,
  .fd      = wsapoll_fd,
  .add     = wsapoll_add,
  .mod     = wsapoll_mod,
  .del     = wsapoll_del,
  .signal  = wsapoll_signal,
};

#endif /* _WIN32 */
