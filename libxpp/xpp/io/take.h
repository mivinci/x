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
 * C++20: coroutine with co_await + co_return.
 * C++11: duck-typed template + single .then() chain.
 */

#ifndef XPP_IO_TAKE_H
#define XPP_IO_TAKE_H

#include <sys/types.h>

#include <cstddef>
#include <utility>

#include <xpp/io/utils.h>

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
 *
 * @tparam R Duck-typed: R::read(void*, size_t) must return a then-able
 *           resolving to ssize_t. C++20 users get AsyncReader concept
 *           inside the #if branch for better error messages.
 */
template <class R> class Take {
public:
  /** @brief Wrap a reader with a byte limit.
   *  @param reader Inner reader (moved in).
   *  @param limit Maximum total bytes to read before returning EOF. */
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
  Promise<ssize_t> read(void *buf, size_t len);

  /** @brief Remaining bytes before EOF. */
  size_t remaining() const { return m_remaining; }

private:
  R      m_reader;
  size_t m_remaining;
};

/* ── read() — two implementations ─────────────────────────────────── */

#if XPP_HAS_COROUTINES

template <class R>
inline Promise<ssize_t> Take<R>::read(void *buf, size_t len) {
  if (m_remaining == 0) co_return 0;
  size_t  limit = len < m_remaining ? len : m_remaining;
  ssize_t n     = co_await m_reader.read(buf, limit);
  if (n > 0) {
    m_remaining -= static_cast<size_t>(n);
  }
  co_return n;
}

#else // !XPP_HAS_COROUTINES

#if XPP_FIBER

/* ═══ C++11 + fiber: linear .await() ═════════════════════════════════ */

template <class R>
inline Promise<ssize_t> Take<R>::read(void *buf, size_t len) {
  if (m_remaining == 0) return xpp::resolve(static_cast<ssize_t>(0));
  size_t  limit = len < m_remaining ? len : m_remaining;
  ssize_t n     = m_reader.read(buf, limit).await();
  if (n > 0) m_remaining -= static_cast<size_t>(n);
  return xpp::resolve(n);
}

#else // !XPP_FIBER

template <class R>
inline Promise<ssize_t> Take<R>::read(void *buf, size_t len) {
  if (m_remaining == 0) return xpp::resolve(static_cast<ssize_t>(0));
  size_t limit = len < m_remaining ? len : m_remaining;
  return m_reader.read(buf, limit).then([this](ssize_t n) {
    if (n > 0) m_remaining -= static_cast<size_t>(n);
    return n;
  });
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

} // namespace io
} // namespace xpp

#endif // XPP_IO_TAKE_H
