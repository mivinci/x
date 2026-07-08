/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bytes.h - xpp::bytes::Bytes: ref-counted immutable byte buffer.
 *
 * Lightweight equivalent of Rust's `bytes::Bytes`. Internally wraps
 * Option<Shared<vector<uint8_t>>> with offset/length for zero-copy slicing.
 *
 * Built from a BytesMut via freeze(), or directly from a vector.
 */

#ifndef XPP_BYTES_BYTES_H
#define XPP_BYTES_BYTES_H

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/shared.h>

namespace xpp {
namespace bytes {

class Bytes {
public:
  Bytes() = default;

  static Bytes from(std::vector<uint8_t> buf) {
    Bytes b;
    b.m_buf =
      Option<Shared<std::vector<uint8_t>>>(Shared<std::vector<uint8_t>>::make(std::move(buf)));
    b.m_len = b.m_buf.as_deref()->size();
    return b;
  }

  /// Copy @p len bytes from @p data into a ref-counted buffer.
  /// Equivalent to `from(std::vector<uint8_t>(data, data + len))`
  /// but avoids the triply-nested constructor chain.
  static Bytes copy(const uint8_t *data, size_t len) {
    std::vector<uint8_t> v(len);
    std::memcpy(v.data(), data, len);
    return from(std::move(v));
  }

  size_t size() const noexcept {
    return m_len;
  }

  const uint8_t *data() const noexcept {
    auto *vec = m_buf.as_deref();
    return vec ? vec->data() + m_offset : nullptr;
  }

  Bytes slice(size_t begin, size_t end) const {
    XPP_DEBUG_ASSERT(begin <= end, "range start must not be greater than end: %zu <= %zu", begin,
                     end);
    XPP_DEBUG_ASSERT(end <= m_len, "range end out of bounds: %zu <= %zu", end, m_len);
    if (begin >= end || begin >= m_len) return Bytes{};
    Bytes ret;
    ret.m_buf    = m_buf;
    ret.m_offset = m_offset + begin;
    ret.m_len    = (end > m_len ? m_len : end) - begin;
    return ret;
  }

  explicit operator std::string_view() const {
    return {reinterpret_cast<const char *>(data()), m_len};
  }

  std::string to_string() const {
    return std::string(reinterpret_cast<const char *>(data()), m_len);
  }

  bool empty() const noexcept {
    return m_len == 0;
  }

  const uint8_t &operator[](size_t i) const {
    return *(data() + i);
  }

private:
  friend class BytesMut;
  Option<Shared<std::vector<uint8_t>>> m_buf;
  size_t                               m_offset = 0;
  size_t                               m_len    = 0;
};

} // namespace bytes
} // namespace xpp

#endif // XPP_BYTES_BYTES_H
