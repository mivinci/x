/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * split.h - xpp::io::split(): split a ReadWriter into read/write halves.
 *
 * Wraps any type satisfying both AsyncReader and AsyncWriter in a
 * Arc<T>, then returns ReadHalf<T> and WriteHalf<T> that
 * share the underlying stream. Like tokio's io::split().
 */
#ifndef XPP_IO_SPLIT_H
#define XPP_IO_SPLIT_H

#include <utility>

#include <xpp/arc.h>
#include <xpp/io/utils.h>
#include <xpp/panic.h>

namespace xpp {
namespace io {

template <AsyncReadWriter T> class WriteHalf;

/** @brief Read half of a split ReadWriter. Satisfies AsyncReader.
 *  @tparam T Inner type satisfying both AsyncReader and AsyncWriter. */
template <AsyncReadWriter T> class ReadHalf {
public:
  ReadHalf()                                = default;
  ReadHalf(ReadHalf &&) noexcept            = default;
  ReadHalf &operator=(ReadHalf &&) noexcept = default;

  /** @brief Forward a read to the shared inner reader.
   *  @param buf Destination buffer.
   *  @param len Maximum bytes to read.
   *  @return Promise resolving to bytes read. */
  Promise<ssize_t> read(void *buf, size_t len) {
    XPP_ASSERT(m_inner, "ReadHalf::read: null inner (default-constructed or moved-from)");
    return m_inner->read(buf, len);
  }

private:
  Arc<T> m_inner;
  explicit ReadHalf(Arc<T> inner) : m_inner(std::move(inner)) {}
  template <AsyncReadWriter U> friend std::pair<ReadHalf<U>, WriteHalf<U>> split(U stream);
};

/** @brief Write half of a split ReadWriter. Satisfies AsyncWriter.
 *  @tparam T Inner type satisfying both AsyncReader and AsyncWriter. */
template <AsyncReadWriter T> class WriteHalf {
public:
  WriteHalf()                                 = default;
  WriteHalf(WriteHalf &&) noexcept            = default;
  WriteHalf &operator=(WriteHalf &&) noexcept = default;

  /** @brief Forward a write to the shared inner writer.
   *  @param buf Source buffer.
   *  @param len Bytes to write.
   *  @return Promise resolving to bytes written. */
  Promise<ssize_t> write(const void *buf, size_t len) {
    XPP_ASSERT(m_inner, "WriteHalf::write: null inner (default-constructed)");
    return m_inner->write(buf, len);
  }
  /** @brief Forward a flush to the shared inner writer.
   *  @return Promise that resolves when the flush completes. */
  Promise<void> flush() {
    XPP_ASSERT(m_inner, "WriteHalf::flush: null inner (default-constructed)");
    return m_inner->flush();
  }

private:
  Arc<T> m_inner;
  explicit WriteHalf(Arc<T> inner) : m_inner(std::move(inner)) {}
  template <AsyncReadWriter U> friend std::pair<ReadHalf<U>, WriteHalf<U>> split(U stream);
};

/** @brief Split a ReadWriter into separate ReadHalf and WriteHalf.
 *  @tparam T Inner type satisfying AsyncReadWriter.
 *  @param stream The stream to split (ownership is shared via Arc<T>).
 *  @return A pair of (read_half, write_half) sharing the same underlying stream. */
template <AsyncReadWriter T> std::pair<ReadHalf<T>, WriteHalf<T>> split(T stream) {
  auto s  = Arc<T>::make(std::move(stream));
  auto rh = ReadHalf<T>(s); // copy first (refcount +1)
  auto wh = WriteHalf<T>(std::move(s));
  return std::make_pair(std::move(rh), std::move(wh));
}

} // namespace io
} // namespace xpp

#endif
