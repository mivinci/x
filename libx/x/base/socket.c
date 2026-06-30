/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket.c - Async socket abstraction over xEventLoop
 */

#include <errno.h>
#include <stdlib.h>

#include <x/base/socket.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#endif

/* ───────────────────── Internal structure ───────────────────── */

struct xSocket_ {
  int          fd;
  xEventLoop   loop;
  xEventSource source;
  xEventMask   mask;
  xSocketFunc  callback;
  void        *userp;
  xTimer       read_timer;
  xTimer       write_timer;
  int          read_timeout_ms;
  int          write_timeout_ms;
};

/* ───────────────────── Forward declarations ───────────────────── */

static void trampoline(int fd, xEventMask mask, void *arg);
static void read_timeout_cb(void *arg);
static void write_timeout_cb(void *arg);
static void reset_read_timer(struct xSocket_ *s);
static void reset_write_timer(struct xSocket_ *s);
static void cancel_read_timer(struct xSocket_ *s);
static void cancel_write_timer(struct xSocket_ *s);

/* ───────────────────── Trampoline ───────────────────── */

static void trampoline(int fd, xEventMask mask, void *arg) {
  struct xSocket_ *s = (struct xSocket_ *)arg;
  (void)fd;

  /* Reset idle timers on normal I/O events */
  if (mask & xEvent_Read) reset_read_timer(s);
  if (mask & xEvent_Write) reset_write_timer(s);

  s->callback((xSocket)s, mask, s->userp);
}

/* ───────────────────── Timeout callbacks ───────────────────── */

static void read_timeout_cb(void *arg) {
  struct xSocket_ *s = (struct xSocket_ *)arg;
  s->read_timer      = NULL;
  /* Or xEvent_Read so user knows which direction timed out */
  s->callback((xSocket)s, xEvent_Timeout | xEvent_Read, s->userp);
}

static void write_timeout_cb(void *arg) {
  struct xSocket_ *s = (struct xSocket_ *)arg;
  s->write_timer     = NULL;
  /* Or xEvent_Write so user knows which direction timed out */
  s->callback((xSocket)s, xEvent_Timeout | xEvent_Write, s->userp);
}

/* ───────────────────── Timer helpers ───────────────────── */

static void cancel_read_timer(struct xSocket_ *s) {
  if (s->read_timer) {
    xTimerStop(s->read_timer);
    s->read_timer = NULL;
  }
}

static void cancel_write_timer(struct xSocket_ *s) {
  if (s->write_timer) {
    xTimerStop(s->write_timer);
    s->write_timer = NULL;
  }
}

static void reset_read_timer(struct xSocket_ *s) {
  if (s->read_timeout_ms <= 0) return;
  cancel_read_timer(s);
  s->read_timer = xTimerStart(read_timeout_cb, s, (uint64_t)s->read_timeout_ms, 0);
}

static void reset_write_timer(struct xSocket_ *s) {
  if (s->write_timeout_ms <= 0) return;
  cancel_write_timer(s);
  s->write_timer = xTimerStart(write_timeout_cb, s, (uint64_t)s->write_timeout_ms, 0);
}

/* ───────────────────── Lifecycle ───────────────────── */

/*
 * socket_open - create a non-blocking, close-on-exec socket fd.
 *
 * On Linux/BSD with SOCK_CLOEXEC/SOCK_NONBLOCK support, both flags are
 * set atomically in the socket() call.  On other platforms we fall back
 * to two separate fcntl() calls.
 *
 * Returns the fd on success, -1 on failure.
 */
static int socket_open(int family, int type, int protocol) {
#ifdef _WIN32
  /* WSASocketW with WSA_FLAG_OVERLAPPED gives us a non-inheritable,
   * non-blocking-capable socket in one call.  WSA_FLAG_NO_HANDLE_INHERIT
   * (Windows 8+) makes the handle non-inheritable; we fall back to
   * SetHandleInformation on older systems. */
  SOCKET sock =
    WSASocketW(family, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  /* If WSA_FLAG_NO_HANDLE_INHERIT failed (older Windows), retry without it */
  if (sock == INVALID_SOCKET) {
    sock = WSASocketW(family, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (sock == INVALID_SOCKET) return -1;
    SetHandleInformation((HANDLE)sock, HANDLE_FLAG_INHERIT, 0);
  }
  /* Set non-blocking mode */
  u_long mode = 1;
  ioctlsocket(sock, FIONBIO, &mode);
  return (int)sock;
#elif defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
  return socket(family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, protocol);
#else
  int fd = socket(family, type, protocol);
  if (fd < 0) return -1;

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto fail;

  int fdflags = fcntl(fd, F_GETFD, 0);
  if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) goto fail;

  return fd;
fail:
  close(fd);
  return -1;
#endif
}

xSocket xSocketCreate(int family, int type, int protocol, xEventMask mask, xSocketFunc callback,
                      void *userp) {
  xEventLoop loop = xEventLoopCurrent();
  if (!loop || !callback) return NULL;

  struct xSocket_ *s = (struct xSocket_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  int fd = socket_open(family, type, protocol);
  if (fd < 0) goto fail;

  xEventSource src = xEventAdd(fd, mask, trampoline, s);
  if (!src) goto fail_fd;

  s->fd       = fd;
  s->loop     = loop;
  s->source   = src;
  s->mask     = mask;
  s->callback = callback;
  s->userp    = userp;

  return (xSocket)s;

fail_fd:
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
fail:
  free(s);
  return NULL;
}

xSocket xSocketCreateFromFd(int fd, xEventMask mask, xSocketFunc callback, void *userp) {
  xEventLoop loop = xEventLoopCurrent();
  if (!loop || !callback || fd < 0) return NULL;

  /* Ensure non-blocking + close-on-exec */
#ifdef _WIN32
  {
    u_long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) return NULL;
    SetHandleInformation((HANDLE)(SOCKET)fd, HANDLE_FLAG_INHERIT, 0);
  }
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return NULL;

  int fdflags = fcntl(fd, F_GETFD, 0);
  if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) return NULL;
#endif

  struct xSocket_ *s = (struct xSocket_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  xEventSource src = xEventAdd(fd, mask, trampoline, s);
  if (!src) {
    free(s);
    return NULL;
  }

  s->fd       = fd;
  s->loop     = loop;
  s->source   = src;
  s->mask     = mask;
  s->callback = callback;
  s->userp    = userp;

  return (xSocket)s;
}

void xSocketDestroy(xSocket sock) {
  if (!sock) return;
  struct xSocket_ *s = (struct xSocket_ *)sock;

  cancel_read_timer(s);
  cancel_write_timer(s);

  xEventDel(s->source);
#ifdef _WIN32
  closesocket(s->fd);
#else
  close(s->fd);
#endif
  free(s);
}

/* ───────────────────── Event mask ───────────────────── */

xErrno xSocketSetMask(xSocket sock, xEventMask mask) {
  if (!sock) return xErrno_InvalidArg;
  struct xSocket_ *s = (struct xSocket_ *)sock;

  xErrno err = xEventMod(s->source, mask);
  if (err == xErrno_Ok) s->mask = mask;
  return err;
}

/* ───────────────────── Timeout ───────────────────── */

xErrno xSocketSetTimeout(xSocket sock, int read_timeout_ms, int write_timeout_ms) {
  if (!sock) return xErrno_InvalidArg;
  struct xSocket_ *s = (struct xSocket_ *)sock;

  /* Read timeout */
  s->read_timeout_ms = read_timeout_ms;
  if (read_timeout_ms > 0) {
    cancel_read_timer(s);
    s->read_timer = xTimerStart(read_timeout_cb, s, (uint64_t)read_timeout_ms, 0);
  } else {
    cancel_read_timer(s);
  }

  /* Write timeout */
  s->write_timeout_ms = write_timeout_ms;
  if (write_timeout_ms > 0) {
    cancel_write_timer(s);
    s->write_timer = xTimerStart(write_timeout_cb, s, (uint64_t)write_timeout_ms, 0);
  } else {
    cancel_write_timer(s);
  }

  return xErrno_Ok;
}

/* ───────────────────── Callback ───────────────────── */

xErrno xSocketSetCallback(xSocket sock, xSocketFunc callback, void *userp) {
  if (!sock || !callback) return xErrno_InvalidArg;
  struct xSocket_ *s = (struct xSocket_ *)sock;
  s->callback        = callback;
  s->userp           = userp;
  return xErrno_Ok;
}

/* ───────────────────── Query ───────────────────── */

int xSocketFd(xSocket sock) {
  if (!sock) return -1;
  return ((struct xSocket_ *)sock)->fd;
}

xEventMask xSocketMask(xSocket sock) {
  if (!sock) return 0;
  return ((struct xSocket_ *)sock)->mask;
}

/* ───────────────────── Datagram I/O ───────────────────── */

ssize_t xSocketSendTo(xSocket sock, const void *buf, size_t len, const struct sockaddr *dest,
                      socklen_t destlen) {
  if (!sock || (!buf && len > 0) || !dest) {
    errno = EINVAL;
    return -1;
  }
  int fd = ((struct xSocket_ *)sock)->fd;
#ifdef _WIN32
  return (ssize_t)sendto(fd, (const char *)buf, (int)len, 0, dest, destlen);
#else
  return sendto(fd, buf, len, 0, dest, destlen);
#endif
}

ssize_t xSocketRecvFrom(xSocket sock, void *buf, size_t len, struct sockaddr *src,
                        socklen_t *srclen) {
  if (!sock || (!buf && len > 0)) {
    errno = EINVAL;
    return -1;
  }
  if (src && !srclen) {
    errno = EINVAL;
    return -1;
  }
  int fd = ((struct xSocket_ *)sock)->fd;
#ifdef _WIN32
  int     fromlen = srclen ? (int)*srclen : 0;
  ssize_t n       = (ssize_t)recvfrom(fd, (char *)buf, (int)len, 0, src, srclen ? &fromlen : NULL);
  if (srclen) *srclen = (socklen_t)fromlen;
  return n;
#else
  return recvfrom(fd, buf, len, 0, src, srclen);
#endif
}
