/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_writer.h - xpp::io::BufWriter<W>: buffered async writer.
 *
 * Wraps any AsyncWriter (TcpStream, fs::File, etc.) with an 8KB
 * internal buffer. Small writes fill the buffer; when it's full,
 * the buffer is automatically flushed to the inner writer. Large
 * writes (≥ 8KB) first flush any pending data, then write directly
 * to the inner writer.
 *
 * Primary use case: building small messages (HTTP responses, framing
 * headers) without triggering a syscall on every write(). Multiple
 * small writes are coalesced into ~8KB chunks.
 *
 * flush() must be called explicitly to send remaining buffered data.
 * The destructor does NOT flush — matching Rust's BufWriter behavior.
 * If you forget to flush(), un-sent data is silently discarded.
 *
 * Composable: BufWriter satisfies the AsyncWriter concept, so it
 * works as the writer in io::copy or nested in another BufWriter.
 * Takes ownership of the inner writer via move semantics.
 *
 * Coroutine-only (C++20). sizeof ≈ 8KB + sizeof(W).
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

/**
 * @brief Buffered async writer.
 *
 * Maintains an 8KB internal buffer (`_::kBufSize`). Small writes
 * accumulate in the buffer. When the buffer would overflow, pending
 * data is automatically flushed to the inner writer. Large writes
 * (≥ 8KB) flush any pending data first, then bypass the buffer.
 *
 * **IMPORTANT**: call `cf flush()` before dropping to send remaining
 * data. The destructor does NOT flush (Rust behavior). Forgot to flush?
 * Data is silently discarded.
 *
 * @tparam W Inner writer type (must satisfy AsyncWriter concept).
 *
 * Usage:
 * @code
 *   auto conn = co_await TcpStream::connect("host:port");
 *   BufWriter<TcpStream> buf(std::move(conn));
 *
 *   // Small writes accumulate — no syscalls yet
 *   co_await buf.write("HTTP/1.0 200 OK\r\n", 17);
 *   co_await buf.write("Content-Length: 5\r\n\r\n", 22);
 *   co_await buf.write("hello", 5);  // might trigger auto-flush
 *
 *   // Must flush before dropping!
 *   co_await buf.flush();
 * @endcode
 */
template <AsyncWriter W> class BufWriter {
public:
  /** @brief Wrap an existing writer (takes ownership). */
  explicit BufWriter(W writer) : m_writer(std::move(writer)) {}

  BufWriter(BufWriter &&) noexcept            = default;
  BufWriter &operator=(BufWriter &&) noexcept = default;
  BufWriter(const BufWriter &)                = delete;
  BufWriter &operator=(const BufWriter &)     = delete;

  /** @brief Destructor does NOT flush un-sent data (Rust behavior). */
  ~BufWriter() = default;

  /**
   * @brief Write up to `len` bytes from `buf`. Satisfies the AsyncWriter
   *        concept, making BufWriter composable with io::copy.
   *
   * Algorithm:
   *   1. If `len >= 8KB`, flush any pending data first, then write
   *      directly to the inner writer (bypass the buffer).
   *   2. If the new data would overflow the buffer, flush first.
   *   3. Copy the new data into the buffer. No I/O unless auto-flush
   *      triggers.
   *
   * @param buf Source buffer (caller-owned, must outlive the Promise).
   * @param len Bytes to write.
   * @return    Promise resolving to bytes written (should equal `len`
   *            unless an error occurs).
   */
  Promise<ssize_t> write(const void *buf, size_t len) {
    // Large write: flush pending data + bypass buffer
    if (len >= _::kBufSize) {
      co_await flush();
      co_return co_await m_writer.write(buf, len);
    }

    // Would overflow: flush pending data first
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
   * MUST be called explicitly before the BufWriter is dropped.
   * The destructor does NOT call flush(). After flush(), the
   * internal buffer is empty and subsequent writes start fresh.
   */
  Promise<void> flush() {
    if (m_pos > 0) {
      co_await m_writer.write(m_buf, m_pos);
      m_pos = 0;
    }
  }

  /** @brief Access the inner writer (non-const). Useful for `close()`. */
  W &inner() {
    return m_writer;
  }

  /** @brief Access the inner writer (const). */
  const W &inner() const {
    return m_writer;
  }

private:
  W       m_writer;           // inner writer (owned)
  uint8_t m_buf[_::kBufSize]; // internal buffer
  size_t  m_pos = 0;          // bytes currently in m_buf
};

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_BUF_WRITER_H
