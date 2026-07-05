/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * split.h - xpp::io::split(): split a ReadWriter into read/write halves.
 *
 * Wraps any type satisfying both AsyncReader and AsyncWriter in a
 * Shared<T>, then returns ReadHalf<T> and WriteHalf<T> that
 * share the underlying stream. Like tokio's io::split().
 */
#ifndef XPP_IO_SPLIT_H
#define XPP_IO_SPLIT_H

#include <utility>

#include <xpp/io/util.h>
#include <xpp/shared.h>

namespace xpp {
namespace io {

template <AsyncReadWriter T> class WriteHalf;

template <AsyncReadWriter T> class ReadHalf {
public:
  ReadHalf()                                = default;
  ReadHalf(ReadHalf &&) noexcept            = default;
  ReadHalf &operator=(ReadHalf &&) noexcept = default;

  Promise<ssize_t> read(void *buf, size_t len) {
    return m_inner->read(buf, len);
  }

private:
  Shared<T> m_inner;
  explicit ReadHalf(Shared<T> inner) : m_inner(std::move(inner)) {}
  template <AsyncReadWriter U> friend std::pair<ReadHalf<U>, WriteHalf<U>> split(U stream);
};

template <AsyncReadWriter T> class WriteHalf {
public:
  WriteHalf()                                 = default;
  WriteHalf(WriteHalf &&) noexcept            = default;
  WriteHalf &operator=(WriteHalf &&) noexcept = default;

  Promise<ssize_t> write(const void *buf, size_t len) {
    return m_inner->write(buf, len);
  }
  Promise<void> flush() {
    return m_inner->flush();
  }

private:
  Shared<T> m_inner;
  explicit WriteHalf(Shared<T> inner) : m_inner(std::move(inner)) {}
  template <AsyncReadWriter U> friend std::pair<ReadHalf<U>, WriteHalf<U>> split(U stream);
};

template <AsyncReadWriter T> std::pair<ReadHalf<T>, WriteHalf<T>> split(T stream) {
  auto s = Shared<T>::make(std::move(stream));
  auto rh = ReadHalf<T>(s);  // copy first (refcount +1)
  auto wh = WriteHalf<T>(std::move(s));
  return std::make_pair(std::move(rh), std::move(wh));
}

} // namespace io
} // namespace xpp

#endif
