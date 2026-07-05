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
#include <vector>

#include <xpp/promise.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace io {

/**
 * @brief Default buffer size for I/O utilities (8KB, matches Rust).
 */

/**
 * @brief Concept: R has read(void*, size_t) returning an awaitable type.
 *
 * Satisfied by TcpStream, fs::File (cursor mode), and user-defined
 * types matching the signature. Used as a template constraint to
 * produce clear compile-time errors when the type is wrong.
 */
template <class R>
concept AsyncReader = requires(R &r, void *buf, size_t len) {
  { r.read(buf, len) };
};

/**
 * @brief Concept: W has write(const void*, size_t) returning an awaitable type.
 */
template <class W>
concept AsyncWriter = requires(W &w, const void *buf, size_t len) {
  { w.write(buf, len) };
};

template <class T>
concept AsyncReadWriter = AsyncReader<T> && AsyncWriter<T>;

namespace _ {

/** @brief Default buffer size for I/O utilities (8KB, matches Rust). */
constexpr size_t kBufSize = 8192;

} // namespace _

/**
 * @brief Read the entire byte stream into a vector.
 *
 * @tparam R Reader type satisfying AsyncReader (e.g., TcpStream, fs::File)
 */
template <AsyncReader R> Promise<std::vector<uint8_t>> read_all(R &reader) {
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
 * @tparam R Reader type satisfying AsyncReader
 * @tparam W Writer type satisfying AsyncWriter
 */
template <AsyncReader R, AsyncWriter W> Promise<void> copy(R &reader, W &writer) {
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
