/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket_test.cpp - Unit tests for xSocket (POSIX)
 */

#include <gtest/gtest.h>

#ifdef _WIN32
TEST(Socket, SkipOnWindows) {
  GTEST_SKIP() << "Socket tests need POSIX adapter";
}
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>

#include <gtest/gtest.h>

#include <x/base/socket.h>

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

static void noop_callback(xSocket, xEventMask, void *) {}

/* Drain all data from a non-blocking fd. */
static void drain_fd(int fd) {
  char buf[256];
  while (read(fd, buf, sizeof(buf)) > 0)
    ;
}

/*
 * Run the event loop until total_ms has elapsed, using a stop timer
 * with X_RUN_DEFAULT to guarantee the full timeout window.
 */
static void pump_loop(xEventLoop loop, int total_ms) {
  xTimer t = xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop,
                         static_cast<uint64_t>(total_ms), 0);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  if (t) xTimerStop(t);
}

/* ───────────────────── SocketCreate ───────────────────── */

TEST(SocketCreate, Success) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  int fd = xSocketFd(sock);
  EXPECT_GE(fd, 0);

  /* Verify O_NONBLOCK */
  int flags = fcntl(fd, F_GETFL, 0);
  EXPECT_TRUE(flags & O_NONBLOCK);

  /* Verify FD_CLOEXEC */
  int fdflags = fcntl(fd, F_GETFD, 0);
  EXPECT_TRUE(fdflags & FD_CLOEXEC);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketCreate, NullLoop) {
  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Read, noop_callback, nullptr);
  EXPECT_EQ(sock, nullptr);
}

TEST(SocketCreate, NullCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Read, NULL, nullptr);
  EXPECT_EQ(sock, nullptr);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketCreate, InvalidFamily) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  /* Use an invalid address family to trigger socket() failure */
  xSocket sock = xSocketCreate(-1, SOCK_STREAM, 0, xEvent_Read, noop_callback, nullptr);
  EXPECT_EQ(sock, nullptr);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketDestroy ───────────────────── */

TEST(SocketDestroy, Normal) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  int fd = xSocketFd(sock);
  ASSERT_GE(fd, 0);

  xSocketDestroy(sock);

  /* Verify fd is closed: fcntl on a closed fd should fail */
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketDestroy, Null) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  /* Must not crash */
  xSocketDestroy(NULL);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketMask (SetMask) ───────────────────── */

TEST(SocketMask, SetAndGet) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  EXPECT_EQ(xSocketMask(sock), xEvent_Read);

  xErrno err = xSocketSetMask(sock, xEvent_Write);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xSocketMask(sock), xEvent_Write);

  err = xSocketSetMask(sock, static_cast<xEventMask>(xEvent_Read | xEvent_Write));
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xSocketMask(sock), (xEventMask)(xEvent_Read | xEvent_Write));

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketMask, InvalidHandle) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  EXPECT_EQ(xSocketSetMask(NULL, xEvent_Read), xErrno_InvalidArg);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketQuery ───────────────────── */

TEST(SocketQuery, Fd) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  int fd = xSocketFd(sock);
  EXPECT_GE(fd, 0);

  /* Verify it's a valid socket fd */
  int       optval;
  socklen_t optlen = sizeof(optval);
  EXPECT_EQ(getsockopt(fd, SOL_SOCKET, SO_TYPE, &optval, &optlen), 0);
  EXPECT_EQ(optval, SOCK_STREAM);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketQuery, Mask) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Write, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  EXPECT_EQ(xSocketMask(sock), xEvent_Write);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketQuery, NullFd) {
  EXPECT_EQ(xSocketFd(NULL), -1);
}

TEST(SocketQuery, NullMask) {
  EXPECT_EQ(xSocketMask(NULL), 0);
}

/* ───────────────────── SocketTimeout ───────────────────── */

TEST(SocketTimeout, ReadTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  struct Ctx {
    xSocket    sock;
    xEventMask mask;
    int        count;
  } ctx = {nullptr, 0, 0};

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket s, xEventMask m, void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      c->sock = s;
      c->mask = m;
      c->count++;
    },
    &ctx);
  ASSERT_NE(sock, nullptr);
  ctx.sock = nullptr;

  /* Set a short read timeout */
  xErrno err = xSocketSetTimeout(sock, 50, 0);
  EXPECT_EQ(err, xErrno_Ok);

  /* Pump the loop until the timeout fires */
  pump_loop(loop, 200);

  EXPECT_GE(ctx.count, 1);
  EXPECT_TRUE(ctx.mask & xEvent_Timeout);
  EXPECT_EQ(ctx.sock, sock);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, WriteTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  struct Ctx {
    xEventMask mask;
    int        timeout_count;
  } ctx = {0, 0};

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      if (m & xEvent_Timeout) {
        c->mask = m;
        c->timeout_count++;
      }
    },
    &ctx);
  ASSERT_NE(sock, nullptr);

  /* Set a short write timeout */
  xErrno err = xSocketSetTimeout(sock, 0, 50);
  EXPECT_EQ(err, xErrno_Ok);

  /* Pump the loop until the timeout fires */
  pump_loop(loop, 200);

  EXPECT_GE(ctx.timeout_count, 1);
  EXPECT_TRUE(ctx.mask & xEvent_Timeout);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, IdleReset) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      if (m & xEvent_Timeout) (*static_cast<int *>(arg))++;
    },
    &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set a 100ms read timeout */
  xSocketSetTimeout(sock, 100, 0);

  /* Pump for 50ms — timeout should not have fired yet */
  pump_loop(loop, 50);
  EXPECT_EQ(timeout_count, 0);

  /* Reset the timer by calling SetTimeout again (simulates idle reset) */
  xSocketSetTimeout(sock, 100, 0);

  /* Pump for 80ms — the reset timer should NOT have fired yet */
  pump_loop(loop, 80);
  EXPECT_EQ(timeout_count, 0);

  /* Pump for another 50ms — now the reset timer (100ms from reset) should fire
   */
  pump_loop(loop, 50);
  EXPECT_EQ(timeout_count, 1);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, CancelWithZero) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      if (m & xEvent_Timeout) (*static_cast<int *>(arg))++;
    },
    &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set a 80ms read timeout, then cancel it */
  xSocketSetTimeout(sock, 80, 0);
  xSocketSetTimeout(sock, 0, 0);

  /* Pump long enough for the original timeout to have fired */
  pump_loop(loop, 150);
  EXPECT_EQ(timeout_count, 0);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, ReplaceTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      if (m & xEvent_Timeout) (*static_cast<int *>(arg))++;
    },
    &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set a 50ms read timeout, then replace with 200ms */
  xSocketSetTimeout(sock, 50, 0);
  xSocketSetTimeout(sock, 200, 0);

  /* Pump 100ms — the original 50ms timeout should NOT fire (replaced) */
  pump_loop(loop, 100);
  EXPECT_EQ(timeout_count, 0);

  /* Pump another 150ms — the 200ms timeout should fire */
  pump_loop(loop, 150);
  EXPECT_EQ(timeout_count, 1);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, DestroyCancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      if (m & xEvent_Timeout) (*static_cast<int *>(arg))++;
    },
    &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set timeouts */
  xSocketSetTimeout(sock, 80, 80);

  /* Destroy before timeouts fire */
  xSocketDestroy(sock);

  /* Pump long enough — timeouts should NOT fire (cancelled by destroy) */
  pump_loop(loop, 200);
  EXPECT_EQ(timeout_count, 0);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketCallback ───────────────────── */

TEST(SocketCallback, HandleMatch) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket received_sock = nullptr;

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket s, xEventMask, void *arg) { *static_cast<xSocket *>(arg) = s; }, &received_sock);
  ASSERT_NE(sock, nullptr);

  /* Use timeout to trigger the callback */
  xSocketSetTimeout(sock, 50, 0);
  pump_loop(loop, 200);

  EXPECT_EQ(received_sock, sock);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketCallback, UserpMatch) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int sentinel = 42;

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask, void *) {
      /* arg IS the userp we passed */
    },
    &sentinel);
  ASSERT_NE(sock, nullptr);

  /* Trigger callback via timeout and verify userp in a different way:
   * use a struct to capture both the arg pointer and a flag */
  struct Ctx {
    void *received_arg;
    int   fired;
  } ctx = {nullptr, 0};

  /* Recreate with a ctx that captures the arg */
  xSocketDestroy(sock);

  sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask, void *arg) {
      auto *c         = static_cast<Ctx *>(arg);
      c->received_arg = arg;
      c->fired        = 1;
    },
    &ctx);
  ASSERT_NE(sock, nullptr);

  xSocketSetTimeout(sock, 50, 0);
  pump_loop(loop, 200);

  EXPECT_EQ(ctx.fired, 1);
  EXPECT_EQ(ctx.received_arg, &ctx);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketCallback, MaskReflectsEvent) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  /* Test 1: Read timeout — mask includes xEvent_Read */
  xEventMask read_timeout_mask = 0;

  xSocket sock_read = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask m, void *arg) { *static_cast<xEventMask *>(arg) = m; },
    &read_timeout_mask);
  ASSERT_NE(sock_read, nullptr);

  xSocketSetTimeout(sock_read, 50, 0);
  pump_loop(loop, 200);

  EXPECT_TRUE(read_timeout_mask & xEvent_Timeout);
  EXPECT_TRUE(read_timeout_mask & xEvent_Read); /* Read timeout => includes Read bit */
  EXPECT_FALSE(read_timeout_mask & xEvent_Write);

  xSocketDestroy(sock_read);

  /* Test 2: Write timeout — mask includes xEvent_Write */
  xEventMask write_timeout_mask = 0;

  xSocket sock_write = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Write,
    [](xSocket, xEventMask m, void *arg) { *static_cast<xEventMask *>(arg) = m; },
    &write_timeout_mask);
  ASSERT_NE(sock_write, nullptr);

  xSocketSetTimeout(sock_write, 0, 50);
  pump_loop(loop, 200);

  EXPECT_TRUE(write_timeout_mask & xEvent_Timeout);
  EXPECT_FALSE(write_timeout_mask & xEvent_Read);
  EXPECT_TRUE(write_timeout_mask & xEvent_Write); /* Write timeout => includes Write bit */

  xSocketDestroy(sock_write);

  /* Test 3: Write event mask — use a socketpair where write end is writable */
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);

  xEventMask io_mask = 0;

  /* Register fds[0] for write — socketpair fds are always writable initially */
  xEventSource src = xEventAdd(
    fds[0], xEvent_Write, [](int, xEventMask m, void *arg) { *static_cast<xEventMask *>(arg) = m; },
    &io_mask);
  ASSERT_NE(src, nullptr);

  xEventLoopRun(loop, X_RUN_ONCE);
  EXPECT_TRUE(io_mask & xEvent_Write);

  xEventDel(src);

  /* Test 4: Read event mask — write data to trigger read */
  xEventMask read_mask = 0;

  xEventSource src2 = xEventAdd(
    fds[0], xEvent_Read,
    [](int fd, xEventMask m, void *arg) {
      *static_cast<xEventMask *>(arg) = m;
      drain_fd(fd);
    },
    &read_mask);
  ASSERT_NE(src2, nullptr);

  write(fds[1], "x", 1);
  xEventLoopRun(loop, X_RUN_ONCE);
  EXPECT_TRUE(read_mask & xEvent_Read);

  xEventDel(src2);
  close(fds[0]);
  close(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketCreateFromFd ───────────────────── */

TEST(SocketCreateFromFd, Success) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  xSocket sock = xSocketCreateFromFd(fds[0], xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  int fd = xSocketFd(sock);
  EXPECT_EQ(fd, fds[0]);

  /* Verify O_NONBLOCK was set */
  int flags = fcntl(fd, F_GETFL, 0);
  EXPECT_TRUE(flags & O_NONBLOCK);

  /* Verify FD_CLOEXEC was set */
  int fdflags = fcntl(fd, F_GETFD, 0);
  EXPECT_TRUE(fdflags & FD_CLOEXEC);

  xSocketDestroy(sock);
  close(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketCreateFromFd, NullLoop) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  xSocket sock = xSocketCreateFromFd(fds[0], xEvent_Read, noop_callback, nullptr);
  EXPECT_EQ(sock, nullptr);

  close(fds[0]);
  close(fds[1]);
}

TEST(SocketCreateFromFd, NullCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  xSocket sock = xSocketCreateFromFd(fds[0], xEvent_Read, NULL, nullptr);
  EXPECT_EQ(sock, nullptr);

  close(fds[0]);
  close(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketCreateFromFd, InvalidFd) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreateFromFd(-1, xEvent_Read, noop_callback, nullptr);
  EXPECT_EQ(sock, nullptr);

  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketCreateFromFd, IOCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  struct Ctx {
    xEventMask mask;
    int        count;
  } ctx = {0, 0};

  xSocket sock = xSocketCreateFromFd(
    fds[0], xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      c->mask = m;
      c->count++;
    },
    &ctx);
  ASSERT_NE(sock, nullptr);

  /* Write data to trigger read event */
  write(fds[1], "hello", 5);
  pump_loop(loop, 100);

  EXPECT_GE(ctx.count, 1);
  EXPECT_TRUE(ctx.mask & xEvent_Read);

  xSocketDestroy(sock);
  close(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketSetCallback ───────────────────── */

TEST(SocketSetCallback, ReplaceCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int original_count = 0;
  int new_count      = 0;

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask, void *arg) { (*static_cast<int *>(arg))++; }, &original_count);
  ASSERT_NE(sock, nullptr);

  /* Replace callback */
  xErrno err = xSocketSetCallback(
    sock, [](xSocket, xEventMask, void *arg) { (*static_cast<int *>(arg))++; }, &new_count);
  EXPECT_EQ(err, xErrno_Ok);

  /* Trigger via timeout */
  xSocketSetTimeout(sock, 50, 0);
  pump_loop(loop, 200);

  EXPECT_EQ(original_count, 0);
  EXPECT_GE(new_count, 1);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketSetCallback, NullSocket) {
  EXPECT_EQ(xSocketSetCallback(NULL, noop_callback, nullptr), xErrno_InvalidArg);
}

TEST(SocketSetCallback, NullCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket sock = xSocketCreate(AF_INET, SOCK_STREAM, 0, xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  EXPECT_EQ(xSocketSetCallback(sock, NULL, nullptr), xErrno_InvalidArg);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketTimeout: NullSocket ───────────────────── */

TEST(SocketTimeout, NullSocket) {
  EXPECT_EQ(xSocketSetTimeout(NULL, 100, 100), xErrno_InvalidArg);
}

/* ───────────────────── SocketTimeout: BothTimeouts ───────────────────── */

TEST(SocketTimeout, BothReadAndWriteTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  struct Ctx {
    int read_timeout_count;
    int write_timeout_count;
  } ctx = {0, 0};

  xSocket sock = xSocketCreate(
    AF_INET, SOCK_STREAM, 0, xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      if ((m & xEvent_Timeout) && (m & xEvent_Read)) c->read_timeout_count++;
      if ((m & xEvent_Timeout) && (m & xEvent_Write)) c->write_timeout_count++;
    },
    &ctx);
  ASSERT_NE(sock, nullptr);

  xSocketSetTimeout(sock, 50, 80);
  pump_loop(loop, 300);

  EXPECT_GE(ctx.read_timeout_count, 1);
  EXPECT_GE(ctx.write_timeout_count, 1);

  xSocketDestroy(sock);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ───────────────────── I/O resets idle timer ───────────────────── */

TEST(SocketTimeout, IOResetsReadTimer) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  struct Ctx {
    int io_count;
    int timeout_count;
    int rfd;
  } ctx = {0, 0, fds[0]};

  xSocket sock = xSocketCreateFromFd(
    fds[0], xEvent_Read,
    [](xSocket, xEventMask m, void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      if (m & xEvent_Timeout) {
        c->timeout_count++;
      } else if (m & xEvent_Read) {
        c->io_count++;
        /* Drain the data */
        char buf[256];
        while (read(c->rfd, buf, sizeof(buf)) > 0)
          ;
      }
    },
    &ctx);
  ASSERT_NE(sock, nullptr);

  /* Set a 150ms read timeout */
  xSocketSetTimeout(sock, 150, 0);

  /* At 50ms, send data — this should trigger read event and reset the timer */
  pump_loop(loop, 50);
  write(fds[1], "hello", 5);
  pump_loop(loop, 20);

  /* The read event should have fired and reset the timer */
  EXPECT_GE(ctx.io_count, 1);
  EXPECT_EQ(ctx.timeout_count, 0);

  /* Wait another 100ms — still within the reset 150ms window */
  pump_loop(loop, 100);
  EXPECT_EQ(ctx.timeout_count, 0);

  /* Wait another 100ms — now the reset timer should fire */
  pump_loop(loop, 100);
  EXPECT_GE(ctx.timeout_count, 1);

  xSocketDestroy(sock);
  close(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, IOResetsWriteTimer) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  struct Ctx {
    int io_count;
    int timeout_count;
  } ctx = {0, 0};

  /* Monitor for write events — socketpair is always writable */
  xSocket sock = xSocketCreateFromFd(
    fds[0], xEvent_Write,
    [](xSocket, xEventMask m, void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      if (m & xEvent_Timeout) {
        c->timeout_count++;
      } else if (m & xEvent_Write) {
        c->io_count++;
      }
    },
    &ctx);
  ASSERT_NE(sock, nullptr);

  /* Set a 100ms write timeout */
  xSocketSetTimeout(sock, 0, 100);

  /* The write event should fire immediately (socketpair is writable),
   * which resets the write timer */
  pump_loop(loop, 50);
  EXPECT_GE(ctx.io_count, 1);

  /* The write timeout should not have fired yet because I/O reset it */
  EXPECT_EQ(ctx.timeout_count, 0);

  xSocketDestroy(sock);
  close(fds[1]);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

#endif /* _WIN32 */

/* ───────────────────── UDP sendto / recvfrom ───────────────────── */

TEST(Socket, UdpSendToRecvFrom) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xSocket a =
    xSocketCreate(AF_INET, SOCK_DGRAM, 0, xEvent_Read, [](xSocket, xEventMask, void *) {}, nullptr);
  xSocket b =
    xSocketCreate(AF_INET, SOCK_DGRAM, 0, xEvent_Read, [](xSocket, xEventMask, void *) {}, nullptr);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  struct sockaddr_in bin;
  memset(&bin, 0, sizeof(bin));
  bin.sin_family      = AF_INET;
  bin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bin.sin_port        = 0;
  ASSERT_EQ(bind(xSocketFd(b), (struct sockaddr *)&bin, sizeof(bin)), 0);
  socklen_t blen = sizeof(bin);
  ASSERT_EQ(getsockname(xSocketFd(b), (struct sockaddr *)&bin, &blen), 0);
  ASSERT_GT(ntohs(bin.sin_port), 0);

  const char *msg = "hello";
  ssize_t     s = xSocketSendTo(a, msg, 5, reinterpret_cast<struct sockaddr *>(&bin), sizeof(bin));
  EXPECT_EQ(s, 5);

  pump_loop(loop, 10);

  char                    rbuf[16] = {0};
  struct sockaddr_storage src;
  socklen_t               srclen = sizeof(src);
  ssize_t                 r =
    xSocketRecvFrom(b, rbuf, sizeof(rbuf) - 1, reinterpret_cast<struct sockaddr *>(&src), &srclen);
  EXPECT_EQ(r, 5);
  EXPECT_STREQ(rbuf, "hello");

  xSocketDestroy(a);
  xSocketDestroy(b);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}
