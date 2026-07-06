/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_reader.h - xpp::io::BufReader<R>: buffered async reader.
 *
 * Wraps any AsyncReader (TcpStream, fs::File, etc.) with an 8KB
 * internal buffer. Small reads copy from the buffer with zero I/O
 * and zero Promise overhead; large reads (≥ 8KB) bypass the buffer
 * entirely and go directly to the inner reader.
 *
 * Primary use case: byte-at-a-time parsing over TCP (HTTP headers,
 * framed protocols) where calling read() directly on TcpStream
 * would create one Promise-chain node per byte. BufReader amortizes
 * these into ~8KB chunks.
 *
 * Composable: BufReader satisfies the AsyncReader concept, so it
 * works with io::read_all, io::copy, or nested in another BufReader.
 * Takes ownership of the inner reader via move semantics.
 *
 * Coroutine-only (C++20). sizeof ≈ 8KB + sizeof(R).
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

/**
 * @brief Buffered async reader.
 *
 * Maintains an 8KB internal buffer (`_::kBufSize`). The first call
 * to read() fills the buffer from the inner reader. Subsequent small
 * reads drain the buffer with zero additional I/O. When the buffer
 * is empty, it refills from the inner reader. Large reads (≥ 8KB)
 * skip the buffer entirely after draining any pending data.
 *
 * @tparam R Inner reader type (must satisfy AsyncReader concept).
 *
 * Usage:
 * @code
 *   auto conn = co_await TcpStream::connect("host:port");
 *   BufReader<TcpStream> buf(std::move(conn));
 *
 *   // Byte-at-a-time parsing with zero per-byte Promise overhead
 *   char c; co_await buf.read(&c, 1);   // fills buffer from TCP
 *   char d; co_await buf.read(&d, 1);   // copies from buffer
 *
 *   // Composable with other io utilities
 *   auto all = co_await io::read_all(buf);
 * @endcode
 */
template <AsyncReader R> class BufReader {
public:
  /** @brief Wrap an existing reader (takes ownership). */
  explicit BufReader(R reader) : m_reader(std::move(reader)) {}

  BufReader(BufReader &&) noexcept            = default;
  BufReader &operator=(BufReader &&) noexcept = default;
  BufReader(const BufReader &)                = delete;
  BufReader &operator=(const BufReader &)     = delete;

  ~BufReader() = default;

  /**
   * @brief Read up to `len` bytes into `buf`. Satisfies the AsyncReader
   *        concept, making BufReader composable with io::read_all/io::copy.
   *
   * Algorithm:
   *   1. If the internal buffer has pending data, copy from it directly
   *      (no I/O, no co_await).
   *   2. If `len >= 8KB`, bypass the buffer: read directly from the inner
   *      reader after draining any remaining buffered data.
   *   3. Otherwise, refill the buffer from the inner reader, then copy.
   *
   * @param buf Destination buffer (caller-owned, must outlive the Promise).
   * @param len Maximum bytes to read.
   * @return    Promise resolving to bytes read (0 = EOF, <0 = error).
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

    // Refill buffer from inner reader
    ssize_t n = co_await m_reader.read(m_buf, _::kBufSize);
    if (n <= 0) co_return n;
    m_filled = static_cast<size_t>(n);

    // Copy requested bytes from freshly filled buffer
    size_t copy = len < m_filled ? len : m_filled;
    std::memcpy(buf, m_buf, copy);
    m_pos = copy;
    co_return static_cast<ssize_t>(copy);
  }

  /** @brief Access the inner reader (non-const). Useful for `close()` or sending. */
  R &inner() {
    return m_reader;
  }

  /** @brief Access the inner reader (const). */
  const R &inner() const {
    return m_reader;
  }

private:
  R       m_reader;           // inner reader (owned)
  uint8_t m_buf[_::kBufSize]; // internal buffer
  size_t  m_pos    = 0;       // next unread position in m_buf
  size_t  m_filled = 0;       // bytes currently in m_buf (invariant: m_pos ≤ m_filled)
};

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_BUF_READER_H
