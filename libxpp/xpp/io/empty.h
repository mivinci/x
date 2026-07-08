/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * empty.h - xpp::io::Empty: always-EOF async reader.
 *
 * Satisfies the AsyncRead concept. read() always returns 0 (EOF).
 * Useful for testing, default values, and placeholder readers.
 */

#ifndef XPP_IO_EMPTY_H
#define XPP_IO_EMPTY_H

#include <sys/types.h>

#include <cstddef>

#include <xpp/promise.h>

namespace xpp {
namespace io {

/** @brief Always-EOF async reader. Satisfies AsyncRead concept. */
struct Empty {
  Promise<ssize_t> read(void *, size_t) {
    return xpp::resolve(static_cast<ssize_t>(0));
  }
};

/** @brief Create an Empty reader (satisfies AsyncRead concept). */
inline Empty empty() {
  return Empty{};
}

} // namespace io
} // namespace xpp

#endif // XPP_IO_EMPTY_H
