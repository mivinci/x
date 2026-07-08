/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bytes_mut.h - xpp::bytes::BytesMut: mutable byte buffer builder.
 *
 * Single-owner buffer for accumulating data before freezing into a
 * shared immutable Bytes. After freeze(), the BytesMut is consumed.
 */

#ifndef XPP_BYTES_BYTES_MUT_H
#define XPP_BYTES_BYTES_MUT_H

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <xpp/bytes/bytes.h>

namespace xpp {
namespace bytes {

class BytesMut {
public:
  BytesMut() = default;

  /**
   * @brief Take ownership of a pre-filled vector.
   */
  static BytesMut from(std::vector<uint8_t> buf) {
    BytesMut b;
    b.m_buf = std::move(buf);
    return b;
  }

  /**
   * @brief Allocate with reserved capacity. Size is still 0.
   */
  static BytesMut with_capacity(size_t cap) {
    BytesMut b;
    b.m_buf.reserve(cap);
    return b;
  }

  size_t size() const noexcept {
    return m_buf.size();
  }
  bool empty() const noexcept {
    return m_buf.empty();
  }

  const uint8_t *data() const noexcept {
    return m_buf.data();
  }

  /**
   * @brief Append raw bytes.
   */
  void put(const uint8_t *src, size_t n) {
    m_buf.insert(m_buf.end(), src, src + n);
  }

  void put(std::string_view s) {
    put(reinterpret_cast<const uint8_t *>(s.data()), s.size());
  }

  /**
   * @brief Consume this BytesMut and return a shared immutable Bytes.
   */
  Bytes freeze() {
    return Bytes::from(std::move(m_buf));
  }

  const uint8_t &operator[](size_t i) const {
    return m_buf[i];
  }
  uint8_t &operator[](size_t i) {
    return m_buf[i];
  }

  explicit operator std::string_view() const {
    return {reinterpret_cast<const char *>(data()), size()};
  }

  std::string to_string() const {
    return std::string(reinterpret_cast<const char *>(data()), size());
  }

private:
  std::vector<uint8_t> m_buf;
};

} // namespace bytes
} // namespace xpp

#endif // XPP_BYTES_BYTES_MUT_H
