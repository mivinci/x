/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_writer.h - xpp::io::BufWriter<W>: buffered async writer.
 *
 * Wraps any AsyncWriter with an 8KB internal buffer. Small writes fill
 * the buffer; when full, auto-flushes to the inner writer. Large writes
 * (≥ 8KB) first flush pending data, then write directly.
 *
 * flush() must be called explicitly to send remaining buffered data.
 * Destructor discards unflushed data (Rust behavior).
 *
 * Satisfies the AsyncWriter concept. Takes ownership of the inner
 * writer via move semantics.
 *
 * Coroutine-only (C++20).
 */

#ifndef XPP_IO_BUF_WRITER_H
#define XPP_IO_BUF_WRITER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/io/util.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace io {

template <AsyncWriter W> class BufWriter {
public:
  explicit BufWriter(W writer) : m_writer(std::move(writer)) {}

  BufWriter(BufWriter &&) noexcept            = default;
  BufWriter &operator=(BufWriter &&) noexcept = default;
  BufWriter(const BufWriter &)                = delete;
  BufWriter &operator=(const BufWriter &)     = delete;

  ~BufWriter() = default; // does NOT flush (Rust behavior)

  /**
   * @brief Buffered write. Fills buffer, auto-flushes when full.
   *
   * Large writes (≥ 8KB) flush any pending data first, then write
   * directly to the inner writer.
   */
  Promise<ssize_t> write(const void *buf, size_t len) {
    // Large write: flush + bypass
    if (len >= _::kBufSize) {
      co_await flush();
      co_return co_await m_writer.write(buf, len);
    }

    // Would overflow: flush first
    if (m_pos + len > _::kBufSize) {
      co_await flush();
    }

    std::memcpy(m_buf + m_pos, buf, len);
    m_pos += len;
    co_return static_cast<ssize_t>(len);
  }

  /**
   * @brief Send all buffered data to the inner writer.
   *
   * Must be called explicitly before dropping the BufWriter.
   * The destructor does NOT call flush().
   */
  Promise<void> flush() {
    if (m_pos > 0) {
      co_await m_writer.write(m_buf, m_pos);
      m_pos = 0;
    }
  }

  W &inner() {
    return m_writer;
  }
  const W &inner() const {
    return m_writer;
  }

private:
  W       m_writer;
  uint8_t m_buf[_::kBufSize];
  size_t  m_pos = 0;
};

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_BUF_WRITER_H
