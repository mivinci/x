/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * util.h - xpp::io utility functions: read_all, copy.
 *
 * Template-based, duck-typed on read(void*, size_t) → Promise<ssize_t>
 * and write(const void*, size_t) → Promise<ssize_t>. No traits, no CRTP.
 * Both functions use co_await loops with stack-allocated 8KB buffers
 * (matches Rust's DEFAULT_BUF_SIZE). C++20 required.
 *
 * C++20-compatible (coroutines). Header-only.
 */

#ifndef XPP_IO_UTIL_H
#define XPP_IO_UTIL_H

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <xpp/promise.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace io {

/**
 * @brief Read the entire byte stream into a vector.
 *
 * Duck-typed: R must have read(void*, size_t) → Promise<ssize_t>.
 * Stops when read returns ≤ 0 (EOF or error). Uses an 8KB stack buffer.
 *
 * @tparam R Reader type (e.g., TcpStream, fs::File)
 */
template <class R> Promise<std::vector<uint8_t>> read_all(R &reader) {
  std::vector<uint8_t> result;
  uint8_t              buf[8192];
  while (true) {
    ssize_t n = co_await reader.read(buf, sizeof(buf));
    if (n <= 0) break;
    result.insert(result.end(), buf, buf + n);
  }
  co_return result;
}

/**
 * @brief Copy all bytes from reader to writer.
 *
 * Duck-typed: R must have read(void*, size_t) → Promise<ssize_t>,
 * W must have write(const void*, size_t) → Promise<ssize_t>.
 * Uses an 8KB stack buffer.
 *
 * @tparam R Reader type
 * @tparam W Writer type
 */
template <class R, class W> Promise<void> copy(R &reader, W &writer) {
  uint8_t buf[8192];
  while (true) {
    ssize_t n = co_await reader.read(buf, sizeof(buf));
    if (n <= 0) co_return;
    co_await writer.write(buf, static_cast<size_t>(n));
  }
}

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_UTIL_H
