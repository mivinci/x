/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sink.h - xpp::io::Sink: write-discarding async writer.
 *
 * Satisfies the AsyncWrite concept. write() always returns len
 * (data is discarded). Useful for benchmarks and /dev/null sinks.
 */

#ifndef XPP_IO_SINK_H
#define XPP_IO_SINK_H

#include <sys/types.h>

#include <cstddef>

#include <xpp/promise.h>

namespace xpp {
namespace io {

/** @brief Write-discarding async writer. Satisfies AsyncWrite concept. */
struct Sink {
  Promise<ssize_t> write(const void *, size_t len) {
    return xpp::resolve(static_cast<ssize_t>(len));
  }
};

/** @brief Create a Sink writer (satisfies AsyncWrite concept). */
inline Sink sink() {
  return Sink{};
}

} // namespace io
} // namespace xpp

#endif // XPP_IO_SINK_H
