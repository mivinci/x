/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_reader.h - xpp::io::BufReader<R>: buffered async reader.
 *
 * Wraps any AsyncReader (TcpStream, fs::File, etc.) with an 8KB
 * internal buffer. Small reads copy from the buffer with zero I/O
 * and zero Promise overhead; large reads (>= 8KB) bypass the buffer
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
 * C++20: coroutine with co_await for buffer refill.
 * C++11: struct + std::move(*this) recursive .then() chain.
 *
 * sizeof(BufReader) = 8 (Shared ptr). The 8KB buffer + inner reader
 * live in a heap-allocated Shared<Inner> to avoid moving bulk state
 * through .then() chain nodes in the C++11 path. In the C++20 path
 * the indirection is a tiny cost for unified member layout.
 */

#ifndef XPP_IO_BUF_READER_H
#define XPP_IO_BUF_READER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/io/utils.h>

namespace xpp {
namespace io {

/**
 * @brief Buffered async reader.
 *
 * Maintains an 8KB internal buffer (`_::kBufSize`). The first call
 * to read() fills the buffer from the inner reader. Subsequent small
 * reads drain the buffer with zero additional I/O. When the buffer
 * is empty, it refills from the inner reader. Large reads (>= 8KB)
 * skip the buffer entirely after draining any pending data.
 *
 * @tparam R Duck-typed: R::read(void*, size_t) must return a then-able
 *           resolving to ssize_t.
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
template <class R> class BufReader {
public:
  /** @brief Wrap an existing reader (takes ownership). */
  explicit BufReader(R reader) {
    m_inner->reader = std::move(reader);
  }

  BufReader(BufReader &&) noexcept            = default;
  BufReader &operator=(BufReader &&) noexcept = default;
  BufReader(const BufReader &)                = delete;
  BufReader &operator=(const BufReader &)     = delete;

  ~BufReader() = default;

  /**
   * @brief Read up to `len` bytes into `buf`.
   *
   * Three paths:
   *   1. Buffered data available → copy + resolve (zero I/O).
   *   2. len >= 8KB → bypass buffer, read directly from inner.
   *   3. Buffer empty → refill from inner reader, then copy.
   */
  Promise<ssize_t> read(void *buf, size_t len);

  /** @brief Access the inner reader (non-const). */
  R &inner() {
    return m_inner->reader;
  }

  /** @brief Access the inner reader (const). */
  const R &inner() const {
    return m_inner->reader;
  }

private:
  struct Inner {
    R       reader;
    uint8_t buf[_::kBufSize];
    size_t  pos    = 0;
    size_t  filled = 0;
  };
  Shared<Inner> m_inner = Shared<Inner>::make();
};

/* ── read() — two implementations ─────────────────────────────────── */

#if XPP_HAS_COROUTINES

/* ═══ C++20: coroutine with co_await ═════════════════════════════════ */

template <class R> inline Promise<ssize_t> BufReader<R>::read(void *buf, size_t len) {
  auto *i = m_inner.get();

  // Path 1: drain remaining buffered data
  if (i->pos < i->filled) {
    size_t avail = i->filled - i->pos;
    size_t n     = len < avail ? len : avail;
    std::memcpy(buf, i->buf + i->pos, n);
    i->pos += n;
    co_return static_cast<ssize_t>(n);
  }
  i->pos    = 0;
  i->filled = 0;

  // Path 2: large read bypasses buffer
  if (len >= _::kBufSize) {
    co_return co_await i->reader.read(buf, len);
  }

  // Path 3: refill buffer from inner reader, then copy
  ssize_t n = co_await i->reader.read(i->buf, _::kBufSize);
  if (n <= 0) co_return n;
  i->filled = static_cast<size_t>(n);

  size_t copy = len < i->filled ? len : i->filled;
  std::memcpy(buf, i->buf, copy);
  i->pos = copy;
  co_return static_cast<ssize_t>(copy);
}

#else // !XPP_HAS_COROUTINES

/* ═══ C++11: struct + std::move(*this) recursive .then() chain ════════
 *
 * Only path 3 (refill) needs the Refill struct — the other two paths
 * resolve immediately. Refill holds Shared<Inner> (8 bytes) + two
 * scalar params, moved through .then() nodes.
 * ─────────────────────────────────────────────────────────────────── */

template <class R> inline Promise<ssize_t> BufReader<R>::read(void *buf, size_t len) {
  auto *i = m_inner.get();

  if (i->pos < i->filled) {
    size_t avail = i->filled - i->pos;
    size_t n     = len < avail ? len : avail;
    std::memcpy(buf, i->buf + i->pos, n);
    i->pos += n;
    return xpp::resolve(static_cast<ssize_t>(n));
  }
  i->pos    = 0;
  i->filled = 0;

  if (len >= _::kBufSize) {
    return i->reader.read(buf, len);
  }

  struct Refill {
    Shared<Inner> inner;
    void         *buf;
    size_t        len;

    Promise<ssize_t> operator()() {
      auto *i = inner.get();
      return i->reader.read(i->buf, _::kBufSize).then([self = std::move(*this)](ssize_t n) mutable {
        if (n <= 0) return xpp::resolve(n);
        self.inner->filled = static_cast<size_t>(n);
        size_t copy        = self.len < self.inner->filled ? self.len : self.inner->filled;
        std::memcpy(self.buf, self.inner->buf, copy);
        self.inner->pos = copy;
        return xpp::resolve(static_cast<ssize_t>(copy));
      });
    }
  };

  return Refill{m_inner, buf, len}();
}

#endif // XPP_HAS_COROUTINES

} // namespace io
} // namespace xpp

#endif // XPP_IO_BUF_READER_H
