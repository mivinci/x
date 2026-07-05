/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * take.h - xpp::io::Take<R>: limited async reader.
 *
 * Wraps an AsyncReader with a byte limit. Returns EOF (0) once the
 * limit is reached, regardless of whether the inner reader has more
 * data. Essential for HTTP Content-Length parsing and protocol frame
 * boundaries.
 *
 * Satisfies the AsyncReader concept. Takes ownership of the inner
 * reader via move semantics.
 *
 * Coroutine-only (C++20).
 */

#ifndef XPP_IO_TAKE_H
#define XPP_IO_TAKE_H

#include <sys/types.h>

#include <cstddef>
#include <utility>

#include <xpp/io/util.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace io {

/**
 * @brief Reader that limits total bytes read from the inner reader.
 *
 * Usages:
 * @code
 *   Take<TcpStream> body(std::move(conn), content_length);
 *   auto data = co_await io::read_all(body);  // exactly content_length bytes
 * @endcode
 */
template <AsyncReader R> class Take {
public:
  Take(R reader, size_t limit) : m_reader(std::move(reader)), m_remaining(limit) {}

  Take(Take &&) noexcept            = default;
  Take &operator=(Take &&) noexcept = default;
  Take(const Take &)                = delete;
  Take &operator=(const Take &)     = delete;

  ~Take() = default;

  /**
   * @brief Read at most `len` bytes, capped by remaining limit.
   *        Returns 0 once the limit is reached (no inner read).
   */
  Promise<ssize_t> read(void *buf, size_t len) {
    if (m_remaining == 0) co_return 0;
    size_t  limit = len < m_remaining ? len : m_remaining;
    ssize_t n     = co_await m_reader.read(buf, limit);
    if (n > 0) {
      m_remaining -= static_cast<size_t>(n);
    }
    co_return n;
  }

  /** @brief Remaining bytes before EOF. */
  size_t remaining() const {
    return m_remaining;
  }

private:
  R      m_reader;
  size_t m_remaining;
};

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_TAKE_H
