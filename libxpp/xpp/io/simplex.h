/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * simplex.h - xpp::io::simplex(): unidirectional in-memory pipe.
 *
 * Returns a reader/writer pair backed by a single ring buffer. Like
 * Go's io.Pipe — write on one end, read on the other. Simpler than
 * duplex() which provides bidirectional communication.
 *
 * Coroutine-only (C++20). Single-threaded.
 */

#ifndef XPP_IO_SIMPLEX_H
#define XPP_IO_SIMPLEX_H

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <xpp/arc.h>
#include <xpp/promise.h>

#if XPP_HAS_COROUTINES

namespace xpp {
namespace io {

namespace _ {

struct SimplexBuf {
  std::vector<uint8_t>  buf;
  size_t                rpos   = 0;
  size_t                wpos   = 0;
  size_t                count  = 0;
  bool                  closed = false;
  PromiseResolver<void> read_waiter;
  PromiseResolver<void> write_waiter;

  explicit SimplexBuf(size_t size) : buf(size) {}
};

} // namespace _

class SimplexWriter; // forward

class SimplexReader {
public:
  Promise<ssize_t> read(void *buf, size_t len) {
    auto *d = m_dup.get();
    if (!d) co_return static_cast<ssize_t>(-1);

    while (d->count == 0) {
      if (d->closed) co_return 0;
      auto pr        = xpp::async<void>();
      d->read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }

    size_t avail = d->count;
    size_t n     = len < avail ? len : avail;
    size_t first = d->rpos;
    if (first + n > d->buf.size()) {
      size_t chunk = d->buf.size() - first;
      std::memcpy(buf, d->buf.data() + first, chunk);
      size_t rem = n - chunk;
      std::memcpy(static_cast<uint8_t *>(buf) + chunk, d->buf.data(), rem);
      d->rpos = rem;
    } else {
      std::memcpy(buf, d->buf.data() + first, n);
      d->rpos = first + n;
    }
    d->count -= n;

    if (d->write_waiter.is_pending()) {
      auto w = std::move(d->write_waiter);
      w.resolve();
    }
    co_return static_cast<ssize_t>(n);
  }

private:
  friend class SimplexWriter;
  friend std::pair<SimplexReader, SimplexWriter> simplex(size_t size);
  Arc<_::SimplexBuf>                             m_dup;
  explicit SimplexReader(Arc<_::SimplexBuf> dup) : m_dup(std::move(dup)) {}
};

class SimplexWriter {
public:
  Promise<ssize_t> write(const void *buf, size_t len) {
    auto *d = m_dup.get();
    if (!d) co_return static_cast<ssize_t>(-1);
    if (len == 0) co_return 0;

    while (d->count + len > d->buf.size()) {
      if (d->closed) co_return static_cast<ssize_t>(-1);
      auto pr         = xpp::async<void>();
      d->write_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }

    const uint8_t *src = static_cast<const uint8_t *>(buf);
    size_t         pos = d->wpos;
    if (pos + len > d->buf.size()) {
      size_t chunk = d->buf.size() - pos;
      std::memcpy(d->buf.data() + pos, src, chunk);
      size_t rem = len - chunk;
      std::memcpy(d->buf.data(), src + chunk, rem);
      d->wpos = rem;
    } else {
      std::memcpy(d->buf.data() + pos, src, len);
      d->wpos = pos + len;
    }
    d->count += len;

    if (d->read_waiter.is_pending()) {
      auto r = std::move(d->read_waiter);
      r.resolve();
    }
    co_return static_cast<ssize_t>(len);
  }

  Promise<void> flush() {
    co_return;
  }

  void close() {
    auto *d = m_dup.get();
    if (!d || d->closed) return;
    d->closed = true;
    if (d->read_waiter.is_pending()) {
      auto r = std::move(d->read_waiter);
      r.resolve();
    }
    if (d->write_waiter.is_pending()) {
      auto w = std::move(d->write_waiter);
      w.resolve();
    }
  }

private:
  friend class SimplexReader;
  friend std::pair<SimplexReader, SimplexWriter> simplex(size_t size);
  Arc<_::SimplexBuf>                             m_dup;
  explicit SimplexWriter(Arc<_::SimplexBuf> dup) : m_dup(std::move(dup)) {}
};

inline std::pair<SimplexReader, SimplexWriter> simplex(size_t size) {
  auto dup = Arc<_::SimplexBuf>::make(size);
  return {SimplexReader(dup), SimplexWriter(std::move(dup))};
}

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_SIMPLEX_H
