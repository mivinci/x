/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * reader.h — xpp::bytes::Reader: buffered reader over Bytes.
 *
 * Reader implements io::TryRead — wraps an immutable Bytes with
 * a cursor, advancing on each try_read() call. Always ready
 * (never returns EAGAIN). Suitable for HTTP request body upload.
 *
 * For streaming scenarios (EAGAIN on empty), mpsc::Receiver<Bytes>
 * also implements io::TryRead natively.
 */

#ifndef XPP_BYTES_READER_H
#define XPP_BYTES_READER_H

#include <cstddef>
#include <cstring>
#include <sys/types.h>

#include <xpp/bytes/bytes.h>
#include <xpp/io/traits.h>

namespace xpp {
namespace bytes {

/* ── Reader — in-memory body (Bytes) ───────────────────────────── */

class Reader {
public:
  explicit Reader(Bytes bytes) : m_bytes(std::move(bytes)) {}

  /// Read at most @p cap bytes into @p buf.  Returns bytes written,
  /// or 0 once all data has been consumed.
  ssize_t try_read(char *buf, size_t cap) {
    size_t remain = m_bytes.size() - m_pos;
    if (remain == 0) return 0;  // EOF
    size_t n = remain < cap ? remain : cap;
    std::memcpy(buf, m_bytes.data() + m_pos, n);
    m_pos += n;
    return static_cast<ssize_t>(n);
  }

private:
  Bytes  m_bytes;
  size_t m_pos = 0;
};

/* ── on_read bridge: TryRead → xHttpReadFunc (curl C callback) ─── */

namespace _ {

/// CURL_READFUNC_PAUSE — returned by the read callback to tell curl
/// to suspend reading and try again later (without this constant curl
/// interprets 0 as EOF).  Defined here to avoid depending on <curl/curl.h>
/// from the header-only xpp layer.
static constexpr size_t kReadFuncPause = static_cast<size_t>(0x10000001);

} // namespace _

} // namespace bytes
} // namespace xpp

#endif // XPP_BYTES_READER_H
