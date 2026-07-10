/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * split.h - xpp::io::split(): split a ReadWriter into read/write halves.
 *
 * Wraps any type satisfying both AsyncRead and AsyncWrite in a
 * Shared<T>, then returns ReadHalf<T> and WriteHalf<T> that
 * share the underlying stream. Like tokio's io::split().
 */
#ifndef XPP_IO_SPLIT_H
#define XPP_IO_SPLIT_H

#include <utility>

#include <xpp/io/utils.h>
#include <xpp/shared.h>

namespace xpp {
namespace io {

template <XPP_REQUIRES_READ_WRITE(T)> class WriteHalf;

/** @brief Read half of a split ReadWriter. Satisfies AsyncRead.
 *  @tparam T Inner type satisfying both AsyncRead and AsyncWrite. */
template <XPP_REQUIRES_READ_WRITE(T)> class ReadHalf {
public:
  ReadHalf()                                = default;
  ReadHalf(ReadHalf &&) noexcept            = default;
  ReadHalf &operator=(ReadHalf &&) noexcept = default;

  /** @brief Forward a read to the shared inner reader.
   *  @param buf Destination buffer.
   *  @param len Maximum bytes to read.
   *  @return Promise resolving to bytes read. */
  Promise<ssize_t> read(void *buf, size_t len) {
    return m_inner->read(buf, len);
  }

private:
  Shared<T> m_inner;
  explicit ReadHalf(Shared<T> inner) : m_inner(std::move(inner)) {}
  template <AsyncReadWrite U> friend std::pair<ReadHalf<U>, WriteHalf<U>> split(U stream);
};

/** @brief Write half of a split ReadWriter. Satisfies AsyncWrite.
 *  @tparam T Inner type satisfying both AsyncRead and AsyncWrite. */
template <XPP_REQUIRES_READ_WRITE(T)> class WriteHalf {
public:
  WriteHalf()                                 = default;
  WriteHalf(WriteHalf &&) noexcept            = default;
  WriteHalf &operator=(WriteHalf &&) noexcept = default;

  /** @brief Forward a write to the shared inner writer.
   *  @param buf Source buffer.
   *  @param len Bytes to write.
   *  @return Promise resolving to bytes written. */
  Promise<ssize_t> write(const void *buf, size_t len) {
    return m_inner->write(buf, len);
  }
  /** @brief Forward a flush to the shared inner writer.
   *  @return Promise that resolves when the flush completes. */
  Promise<void> flush() {
    return m_inner->flush();
  }

private:
  Shared<T> m_inner;
  explicit WriteHalf(Shared<T> inner) : m_inner(std::move(inner)) {}
  template <AsyncReadWrite U> friend std::pair<ReadHalf<U>, WriteHalf<U>> split(U stream);
};

/** @brief Split a ReadWriter into separate ReadHalf and WriteHalf.
 *  @tparam T Inner type satisfying AsyncReadWrite.
 *  @param stream The stream to split (ownership is shared via Shared<T>).
 *  @return A pair of (read_half, write_half) sharing the same underlying stream. */
template <XPP_REQUIRES_READ_WRITE(T)> std::pair<ReadHalf<T>, WriteHalf<T>> split(T stream) {
  auto s = Shared<T>::make(std::move(stream));
  auto rh = ReadHalf<T>(s);  // copy first (refcount +1)
  auto wh = WriteHalf<T>(std::move(s));
  return std::make_pair(std::move(rh), std::move(wh));
}

} // namespace io
} // namespace xpp

#endif
