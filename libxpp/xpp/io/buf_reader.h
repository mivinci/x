/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_reader.h - xpp::io::BufReader<R>: buffered async reader.
 *
 * Wraps any AsyncReader with an 8KB internal buffer. Small reads copy
 * from the buffer; large reads (≥ 8KB) bypass it. Reduces per-call
 * Promise overhead for byte-at-a-time parsing.
 *
 * Satisfies the AsyncReader concept — composable with io::read_all.
 * Takes ownership of the inner reader via move semantics.
 *
 * Coroutine-only (C++20).
 */

#ifndef XPP_IO_BUF_READER_H
#define XPP_IO_BUF_READER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/io/util.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace io {

template <AsyncReader R> class BufReader {
public:
  explicit BufReader(R reader) : m_reader(std::move(reader)) {}

  BufReader(BufReader &&) noexcept            = default;
  BufReader &operator=(BufReader &&) noexcept = default;
  BufReader(const BufReader &)                = delete;
  BufReader &operator=(const BufReader &)     = delete;

  ~BufReader() = default;

  /**
   * @brief Buffered read. Drains buffer first, refills from inner when empty.
   *
   * Large reads (≥ 8KB) go directly to the inner reader after draining
   * any pending buffered data.
   */
  Promise<ssize_t> read(void *buf, size_t len) {
    // Drain remaining buffered data first
    if (m_pos < m_filled) {
      size_t avail = m_filled - m_pos;
      size_t n     = len < avail ? len : avail;
      std::memcpy(buf, m_buf + m_pos, n);
      m_pos += n;
      co_return static_cast<ssize_t>(n);
    }
    m_pos    = 0;
    m_filled = 0;

    // Large read: bypass buffer
    if (len >= _::kBufSize) {
      co_return co_await m_reader.read(buf, len);
    }

    // Refill buffer
    ssize_t n = co_await m_reader.read(m_buf, _::kBufSize);
    if (n <= 0) co_return n;
    m_filled = static_cast<size_t>(n);

    // Copy to output
    size_t copy = len < m_filled ? len : m_filled;
    std::memcpy(buf, m_buf, copy);
    m_pos = copy;
    co_return static_cast<ssize_t>(copy);
  }

  R &inner() {
    return m_reader;
  }
  const R &inner() const {
    return m_reader;
  }

private:
  R       m_reader;
  uint8_t m_buf[_::kBufSize];
  size_t  m_pos    = 0;
  size_t  m_filled = 0;
};

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_BUF_READER_H
