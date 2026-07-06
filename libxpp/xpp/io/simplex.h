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
 * C++20: coroutine with while + co_await loops.
 * C++11: struct + std::move(*this) recursive .then() chain.
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
#include <xpp/shared.h>

namespace xpp {
namespace io {

/* ── SimplexBuf ────────────────────────────────────────────────────── */

namespace _ {

/** @brief Ring buffer backing a simplex pipe. Pure data — no coroutine dep. */
struct SimplexBuf {
  std::vector<uint8_t>  buf;            ///< Underlying byte storage.
  size_t                rpos   = 0;     ///< Next read position (circular).
  size_t                wpos   = 0;     ///< Next write position (circular).
  size_t                count  = 0;     ///< Bytes currently buffered.
  bool                  closed = false; ///< Whether the write end is closed.
  PromiseResolver<void> read_waiter;    ///< Resolves when data is available.
  PromiseResolver<void> write_waiter;   ///< Resolves when space is available.

  explicit SimplexBuf(size_t size) : buf(size) {}
};

} // namespace _

/* ── Class declarations ─────────────────────────────────────────────── */

class SimplexWriter;

/**
 * @brief Read half of a simplex pipe. Reads data written by SimplexWriter.
 *
 * Single class definition shared by both C++20 and C++11 builds.
 * m_dup uses xpp::Shared (Rc by default, Arc with XPP_MT) — a simplex
 * pipe is single-threaded, so atomic overhead in Arc is wasted.
 */
class SimplexReader {
public:
  /** @brief Read up to len bytes into buf. Blocks when empty. */
  Promise<ssize_t> read(void *buf, size_t len);

private:
  friend class SimplexWriter;
  friend std::pair<SimplexReader, SimplexWriter> simplex(size_t size);
  Shared<_::SimplexBuf>                          m_dup;
  explicit SimplexReader(Shared<_::SimplexBuf> dup) : m_dup(std::move(dup)) {}
};

/**
 * @brief Write half of a simplex pipe. Data written becomes readable.
 */
class SimplexWriter {
public:
  /** @brief Write up to len bytes from buf. Blocks when full. */
  Promise<ssize_t> write(const void *buf, size_t len);

  /** @brief No-op — simplex data is always "flushed" on write. */
  Promise<void> flush() { return xpp::resolve(); }

  /** @brief Close the write end, waking blocked readers/writers. */
  void close();

private:
  friend class SimplexReader;
  friend std::pair<SimplexReader, SimplexWriter> simplex(size_t size);
  Shared<_::SimplexBuf>                          m_dup;
  explicit SimplexWriter(Shared<_::SimplexBuf> dup) : m_dup(std::move(dup)) {}
};

/* ── Shared methods (no coroutine dep) ──────────────────────────────── */

inline void SimplexWriter::close() {
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

/* ── read / write — two implementations ─────────────────────────────── */

#if XPP_HAS_COROUTINES

/* ═══ C++20: coroutine with while + co_await ══════════════════════════ */

inline Promise<ssize_t> SimplexReader::read(void *buf, size_t len) {
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

inline Promise<ssize_t> SimplexWriter::write(const void *buf, size_t len) {
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

#else // !XPP_HAS_COROUTINES

/* ═══ C++11: struct + std::move(*this) recursive .then() chain ════════
 *
 * Replaces `while (condition) { co_await ... }` with a local recursive
 * struct whose operator() chains .then() on fresh Promise<void> instances.
 *
 * ReadLoop/WriteLoop hold Shared<T> (8B–16B) + 2 scalars, moved through
 * .then() nodes via std::move(*this). Zero heap allocation per retry —
 * only the PromiseNode arena bump alloc.
 * ─────────────────────────────────────────────────────────────────── */

inline Promise<ssize_t> SimplexReader::read(void *buf, size_t len) {
  struct ReadLoop {
    Shared<_::SimplexBuf> dup;
    void                 *buf;
    size_t                len;

    Promise<ssize_t> operator()() {
      auto *d = dup.get();
      if (!d) return xpp::resolve(static_cast<ssize_t>(-1));

      if (d->count > 0) {
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
        return xpp::resolve(static_cast<ssize_t>(n));
      }

      if (d->closed) return xpp::resolve(0);

      auto pr        = xpp::async<void>();
      d->read_waiter = std::move(pr.second);
      return std::move(pr.first).then([self = std::move(*this)](Void) mutable {
        return self();
      });
    }
  };

  return ReadLoop{m_dup, buf, len}();
}

inline Promise<ssize_t> SimplexWriter::write(const void *buf, size_t len) {
  struct WriteLoop {
    Shared<_::SimplexBuf> dup;
    const void           *buf;
    size_t                len;

    Promise<ssize_t> operator()() {
      auto *d = dup.get();
      if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
      if (len == 0) return xpp::resolve(0);

      if (d->count + len <= d->buf.size()) {
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
        return xpp::resolve(static_cast<ssize_t>(len));
      }

      if (d->closed) return xpp::resolve(static_cast<ssize_t>(-1));

      auto pr         = xpp::async<void>();
      d->write_waiter = std::move(pr.second);
      return std::move(pr.first).then([self = std::move(*this)](Void) mutable {
        return self();
      });
    }
  };

  return WriteLoop{m_dup, buf, len}();
}

#endif // XPP_HAS_COROUTINES

/* ── Factory (shared — both branches use Shared) ───────────────────── */

inline std::pair<SimplexReader, SimplexWriter> simplex(size_t size) {
  auto dup = Shared<_::SimplexBuf>::make(size);
  return {SimplexReader(dup), SimplexWriter(std::move(dup))};
}

} // namespace io
} // namespace xpp

#endif // XPP_IO_SIMPLEX_H
