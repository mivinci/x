/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * random.h - Cross-platform cryptographically secure random bytes
 */

#ifndef XBASE_RANDOM_H
#define XBASE_RANDOM_H

#include <stddef.h>
#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief Fill @p buf with @p len cryptographically secure random bytes.
 *
 * Uses platform-native APIs: getrandom() on Linux, getentropy() on
 * macOS, BCryptGenRandom() on Windows. Falls back to /dev/urandom.
 *
 * @param buf  Destination buffer (must not be NULL).
 * @param len  Number of bytes to generate.
 * @return     xErrno_Ok on success, xErrno_InvalidArg if buf is NULL.
 */
XCAPI(xErrno) xRandomBytes(void *buf, size_t len);

#endif /* XBASE_RANDOM_H */
