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
 * writes (>= 8KB) first flush any pending data, then write directly
 * to the inner writer.
 *
 * Primary use case: building small messages (HTTP responses, framing
 * headers) without triggering a syscall on every write(). Multiple
 * small writes are coalesced into ~8KB chunks.
 *
 * flush() must be called explicitly to send remaining buffered data.
 * The destructor does NOT flush — matching Rust's BufWriter behavior.
 *
 * Composable: BufWriter satisfies the AsyncWriter concept, so it
 * works as the writer in io::copy or nested in another BufWriter.
 * Takes ownership of the inner writer via move semantics.
 *
 * C++20: coroutine with co_await for auto-flush.
 * C++11: struct + std::move(*this) .then() chains.
 *
 * sizeof(BufWriter) = 8 (Shared ptr). 8KB buffer + inner writer live
 * in heap-allocated Shared<Inner>.
 */

#ifndef XPP_IO_BUF_WRITER_H
#define XPP_IO_BUF_WRITER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/io/utils.h>

namespace xpp {
namespace io {

/**
 * @brief Buffered async writer.
 *
 * Maintains an 8KB internal buffer (`_::kBufSize`). Small writes
 * accumulate in the buffer. When the buffer would overflow, pending
 * data is automatically flushed to the inner writer. Large writes
 * (>= 8KB) flush any pending data first, then bypass the buffer.
 *
 * **IMPORTANT**: call `co_await flush()` before dropping to send
 * remaining data. The destructor does NOT flush (Rust behavior).
 *
 * @tparam W Duck-typed: W::write(const void*, size_t) must return a
 *           then-able resolving to ssize_t.
 */
template <class W> class BufWriter {
public:
  /** @brief Wrap an existing writer (takes ownership). */
  explicit BufWriter(W writer) {
    m_inner->writer = std::move(writer);
  }

  BufWriter(BufWriter &&) noexcept            = default;
  BufWriter &operator=(BufWriter &&) noexcept = default;
  BufWriter(const BufWriter &)                = delete;
  BufWriter &operator=(const BufWriter &)     = delete;

  /** @brief Destructor does NOT flush un-sent data. */
  ~BufWriter() = default;

  /**
   * @brief Write up to `len` bytes from `buf`. Three paths:
   *   1. len >= 8KB → flush pending + write directly.
   *   2. Would overflow → flush first, then buffer.
   *   3. Fits in buffer → memcpy, zero I/O.
   */
  Promise<ssize_t> write(const void *buf, size_t len);

  /**
   * @brief Send all buffered data to the inner writer.
   *
   * MUST be called explicitly before dropping. The destructor does
   * NOT call flush(). After flush(), the buffer is empty.
   */
  Promise<void> flush();

  /** @brief Access the inner writer (non-const). */
  W &inner() {
    return m_inner->writer;
  }

  /** @brief Access the inner writer (const). */
  const W &inner() const {
    return m_inner->writer;
  }

private:
  struct Inner {
    W       writer;
    uint8_t buf[_::kBufSize];
    size_t  pos = 0;
  };
  Shared<Inner> m_inner = Shared<Inner>::make();
};

/* ── write() / flush() — two implementations ──────────────────────── */

#if XPP_HAS_COROUTINES

/* ═══ C++20: coroutine with co_await ═════════════════════════════════ */

template <class W> inline Promise<ssize_t> BufWriter<W>::write(const void *buf, size_t len) {
  auto *i = m_inner.get();

  if (len >= _::kBufSize) {
    co_await flush();
    co_return co_await i->writer.write(buf, len);
  }

  if (i->pos + len > _::kBufSize) {
    co_await flush();
  }

  std::memcpy(i->buf + i->pos, buf, len);
  i->pos += len;
  co_return static_cast<ssize_t>(len);
}

template <class W> inline Promise<void> BufWriter<W>::flush() {
  auto *i = m_inner.get();
  if (i->pos > 0) {
    co_await i->writer.write(i->buf, i->pos);
    i->pos = 0;
  }
}

#else // !XPP_HAS_COROUTINES

#if XPP_FIBER

/* ═══ C++11 + fiber: linear .await() ═════════════════════════════════ */

template <class W> inline Promise<ssize_t> BufWriter<W>::write(const void *buf, size_t len) {
  auto *i = m_inner.get();

  if (len >= _::kBufSize) {
    if (i->pos > 0) flush().await();
    return i->writer.write(buf, len);
  }

  if (i->pos + len > _::kBufSize) {
    flush().await();
  }

  std::memcpy(i->buf + i->pos, buf, len);
  i->pos += len;
  return xpp::resolve(static_cast<ssize_t>(len));
}

template <class W> inline Promise<void> BufWriter<W>::flush() {
  auto *i = m_inner.get();
  if (i->pos == 0) return xpp::resolve();
  i->writer.write(i->buf, i->pos).await();
  i->pos = 0;
  return xpp::resolve();
}

#else // !XPP_FIBER

/* ═══ C++11: .then() chains (no while loop, so no struct+move needed) ══ */

template <class W> inline Promise<ssize_t> BufWriter<W>::write(const void *buf, size_t len) {
  auto *i = m_inner.get();

  // Path 1: large write — flush pending then bypass buffer
  if (len >= _::kBufSize) {
    if (i->pos == 0) return i->writer.write(buf, len);
    return i->writer.write(i->buf, i->pos).then([inner = m_inner, buf, len](ssize_t) {
      inner->pos = 0;
      return inner->writer.write(buf, len);
    });
  }

  // Path 2: would overflow — flush pending data first
  if (i->pos + len > _::kBufSize) {
    return i->writer.write(i->buf, i->pos).then([inner = m_inner, buf, len](ssize_t) {
      inner->pos = 0;
      std::memcpy(inner->buf, buf, len);
      inner->pos = len;
      return xpp::resolve(static_cast<ssize_t>(len));
    });
  }

  // Path 3: normal — buffer the write, zero I/O
  std::memcpy(i->buf + i->pos, buf, len);
  i->pos += len;
  return xpp::resolve(static_cast<ssize_t>(len));
}

template <class W> inline Promise<void> BufWriter<W>::flush() {
  auto *i = m_inner.get();
  if (i->pos == 0) return xpp::resolve();
  return i->writer.write(i->buf, i->pos).then([inner = m_inner](ssize_t) {
    inner->pos = 0;
    return xpp::resolve();
  });
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

} // namespace io
} // namespace xpp

#endif // XPP_IO_BUF_WRITER_H
