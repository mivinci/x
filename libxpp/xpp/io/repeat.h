/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * repeat.h - xpp::io::Repeat: infinite repeating byte reader.
 *
 * read() always returns the same byte. Never returns EOF.
 * Useful for benchmarks and generating padding data.
 */
#ifndef XPP_IO_REPEAT_H
#define XPP_IO_REPEAT_H

#include <sys/types.h>

#include <cstddef>
#include <cstring>

#include <xpp/promise.h>

namespace xpp {
namespace io {

/** @brief Infinite repeater: read() always returns the same byte. Never EOF. */
class Repeat {
public:
  /** @brief Create a repeater that fills buffers with the given byte.
   *  @param b The byte to repeat. */
  explicit Repeat(uint8_t b) : m_byte(b) {}

  /** @brief Fill buf with len copies of the repeat byte.
   *  @param buf Destination buffer.
   *  @param len Number of bytes to fill.
   *  @return Promise always resolving to len (never returns 0/EOF). */
  Promise<ssize_t> read(void *buf, size_t len) {
    std::memset(buf, m_byte, len);
    return xpp::resolve(static_cast<ssize_t>(len));
  }

private:
  uint8_t m_byte;
};

/** @brief Construct a Repeat reader that fills buffers with the given byte.
 *  @param byte The byte to repeat (default 0). */
inline Repeat repeat(uint8_t byte = 0) {
  return Repeat{byte};
}

} // namespace io
} // namespace xpp
#endif
