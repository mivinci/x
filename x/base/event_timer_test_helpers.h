/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_timer_test_helpers.h — Shared helpers for timer tests
 */

#ifndef XBASE_EVENT_TIMER_TEST_HELPERS_H
#define XBASE_EVENT_TIMER_TEST_HELPERS_H

#include <x/base/event.h>

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

using ms = std::chrono::milliseconds;

static inline void sleep_ms(int n) {
  std::this_thread::sleep_for(ms(n));
}

static inline int make_pipe(int fds[2]) {
#ifdef _WIN32
  static bool wsa_init = false;
  if (!wsa_init) {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    wsa_init = true;
  }
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return -1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(listener);
    return -1;
  }
  if (listen(listener, 1) != 0) {
    closesocket(listener);
    return -1;
  }
  int addrlen = sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) {
    closesocket(listener);
    return -1;
  }
  SOCKET conn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (conn == INVALID_SOCKET) {
    closesocket(listener);
    return -1;
  }
  if (connect(conn, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(listener);
    closesocket(conn);
    return -1;
  }
  SOCKET acceptor = accept(listener, NULL, NULL);
  closesocket(listener);
  if (acceptor == INVALID_SOCKET) {
    closesocket(conn);
    return -1;
  }
  u_long mode = 1;
  ioctlsocket(acceptor, FIONBIO, &mode);
  ioctlsocket(conn, FIONBIO, &mode);
  fds[0] = (int)acceptor;
  fds[1] = (int)conn;
  return 0;
#else
  if (pipe(fds) != 0) return -1;
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
  return 0;
#endif
}

static inline void close_fd(int fd) {
#ifdef _WIN32
  closesocket((SOCKET)fd);
#else
  close(fd);
#endif
}

static inline void drain_fd(int fd) {
  char buf[256];
#ifdef _WIN32
  while (recv((SOCKET)fd, buf, sizeof(buf), 0) > 0)
    ;
#else
  while (read(fd, buf, sizeof(buf)) > 0)
    ;
#endif
}

static inline void write_fd(int fd, const char *data, size_t len) {
#ifdef _WIN32
  send((SOCKET)fd, data, (int)len, 0);
#else
  write(fd, data, len);
#endif
}

#endif /* XBASE_EVENT_TIMER_TEST_HELPERS_H */
