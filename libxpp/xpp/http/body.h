/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * body.h — TryReader concept + built-in body readers.
 *
 * A TryReader is anything that implements:
 *   ssize_t try_read(char *buf, size_t cap)
 *
 * Return semantics mirror read(2):
 *   > 0  — bytes written to buf
 *   == 0 — EOF (no more data)
 *   < 0  — EAGAIN (data is being produced, try again later)
 *
 * The library provides:
 *   SliceReader  — wraps in-memory data (vector/Bytes), always ready
 *   mpsc::Receiver<Bytes> — implements TryReader natively
 */

#ifndef XPP_HTTP_BODY_H
#define XPP_HTTP_BODY_H

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <sys/types.h>
#include <vector>

#include <xpp/compiler.h>
#include <xpp/bytes/bytes.h>

namespace xpp {
namespace http {

/* ── C++20 concept (production) ─────────────────────────────────── */

#if XPP_HAS_CONCEPT

#include <concepts>

template <class R>
concept TryReader = requires(R &r, char *buf, size_t cap) {
  // Must return ssize_t: > 0 = data, 0 = EOF, < 0 = EAGAIN
  { r.try_read(buf, cap) } -> std::same_as<ssize_t>;
};

#endif // XPP_HAS_CONCEPT

/* ── SliceReader — in-memory body (vector / Bytes) ──────────────── */

class SliceReader {
public:
  /// Wraps a vector<uint8_t> — data must outlive the request.
  explicit SliceReader(const std::vector<uint8_t> &data)
      : m_data(data.data()), m_len(data.size()) {}

  /// Wraps raw memory.
  SliceReader(const uint8_t *data, size_t len)
      : m_data(data), m_len(len) {}

  /// Read at most @p cap bytes into @p buf.  Returns bytes written,
  /// or 0 once all data has been consumed.
  ssize_t try_read(char *buf, size_t cap) {
    if (m_pos >= m_len) return 0;  // EOF
    size_t n = m_len - m_pos;
    if (n > cap) n = cap;
    std::memcpy(buf, m_data + m_pos, n);
    m_pos += n;
    return static_cast<ssize_t>(n);
  }

private:
  const uint8_t *m_data;
  size_t         m_len;
  size_t         m_pos = 0;
};

/* ── on_read bridge: TryReader → xHttpReadFunc (curl C callback) ── */

namespace _ {

/// CURL_READFUNC_PAUSE — returned by the read callback to tell curl
/// to suspend reading and try again later (without this constant curl
/// interprets 0 as EOF).  Defined here to avoid depending on <curl/curl.h>
/// from the header-only xpp layer.
static constexpr size_t kReadFuncPause = static_cast<size_t>(0x10000001);

} // namespace _

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_BODY_H
