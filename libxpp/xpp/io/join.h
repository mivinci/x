/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * join.h - xpp::io::join(): combine a reader and writer into one type.
 *
 * Joins an AsyncReader and AsyncWriter into a single type satisfying
 * both concepts. Useful when you have separate read/write halves
 * (e.g., from simplex, or separate TcpStreams for each direction).
 *
 * C++20 (depends on concepts from util.h). No coroutines needed —
 * read/write simply forward to the inner reader/writer.
 */
#ifndef XPP_IO_JOIN_H
#define XPP_IO_JOIN_H

#include <sys/types.h>
#include <utility>

#include <xpp/promise.h>

namespace xpp {
namespace io {

template <class R, class W>
class Join {
public:
  Join(R reader, W writer) : m_reader(std::move(reader)), m_writer(std::move(writer)) {}

  Join(Join &&) noexcept            = default;
  Join &operator=(Join &&) noexcept = default;
  Join(const Join &)                = delete;
  Join &operator=(const Join &)     = delete;

  Promise<ssize_t> read(void *buf, size_t len) {
    return m_reader.read(buf, len);
  }

  Promise<ssize_t> write(const void *buf, size_t len) {
    return m_writer.write(buf, len);
  }

  Promise<void> flush() { return m_writer.flush(); }
  void close() { m_writer.close(); }

private:
  R m_reader;
  W m_writer;
};

/** @brief Join a reader and writer into a single bidirectional type. */
template <class R, class W>
Join<R, W> join(R reader, W writer) {
  return Join<R, W>(std::move(reader), std::move(writer));
}

} // namespace io
} // namespace xpp

#endif
