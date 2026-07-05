/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_helpers.h — Shared helpers for net tests.
 */
#ifndef XPP_NET_TEST_HELPERS_H
#define XPP_NET_TEST_HELPERS_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>

/** @brief Bind to port 0 and return the assigned port. Returns 0 on failure. */
inline uint16_t get_free_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in addr = {};
  addr.sin_family         = AF_INET;
  addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
  addr.sin_port           = 0;
  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len);
  uint16_t port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

#endif // XPP_NET_TEST_HELPERS_H
