/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * random.c - Cross-platform cryptographically secure random bytes
 */

#include <x/base/random.h>

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <sys/random.h>
#endif
#endif

xErrno xRandomBytes(void *buf, size_t len) {
  if (!buf) return xErrno_InvalidArg;
  if (len == 0) return xErrno_Ok;

#ifdef _WIN32
  NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) return xErrno_SysError;
  return xErrno_Ok;

#elif defined(__linux__) && defined(SYS_getrandom)
  /* getrandom() — Linux 3.17+ */
  char  *p   = (char *)buf;
  size_t rem = len;
  while (rem > 0) {
    ssize_t n = syscall(SYS_getrandom, p, rem, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break; /* fall through to /dev/urandom */
    }
    p += n;
    rem -= (size_t)n;
  }
  if (rem == 0) return xErrno_Ok;

#elif defined(__APPLE__)
  /* getentropy() — macOS 10.12+ */
  if (getentropy(buf, len) == 0) return xErrno_Ok;
#endif

  /* Fallback: /dev/urandom */
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return xErrno_SysError;

  char  *p   = (char *)buf;
  size_t rem = len;
  while (rem > 0) {
    ssize_t n = read(fd, p, rem);
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return xErrno_SysError;
    }
    if (n == 0) break;
    p += n;
    rem -= (size_t)n;
  }
  close(fd);
  return (rem == 0) ? xErrno_Ok : xErrno_SysError;
}
