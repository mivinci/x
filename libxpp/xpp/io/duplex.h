/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * duplex.h - xpp::io::duplex(): in-memory bidirectional pipe.
 *
 * Returns a pair of DuplexStreams connected by two internal ring
 * buffers. Each half satisfies both AsyncReader and AsyncWriter —
 * writing to one side makes data readable on the other, emulating
 * a pair of connected sockets.
 *
 * Like tokio's DuplexStream or Go's net.Pipe.
 *
 * C++20: coroutine with while + co_await loops.
 * C++11: struct + std::move(*this) recursive .then() chain.
 */

#ifndef XPP_IO_DUPLEX_H
#define XPP_IO_DUPLEX_H

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

/* ── DuplexBuf ─────────────────────────────────────────────────────── */

namespace _ {

/** @brief Two ring buffers, one per direction. Pure data — no coroutine dep. */
struct DuplexBuf {
  struct Side {
    std::vector<uint8_t>  buf;
    size_t                rpos   = 0;
    size_t                wpos   = 0;
    size_t                count  = 0;
    bool                  closed = false;
    PromiseResolver<void> read_waiter;
    PromiseResolver<void> write_waiter;
  };
  Side side[2];

  explicit DuplexBuf(size_t size) {
    side[0].buf.resize(size);
    side[1].buf.resize(size);
  }
};

} // namespace _

/* ── Class declaration ─────────────────────────────────────────────── */

/**
 * @brief One half of a duplex pipe. Satisfies AsyncReader and AsyncWriter.
 *
 * Writing to this side makes data readable on the other side, and
 * vice versa — like a connected socket pair. Each side has its own
 * internal ring buffer for incoming data.
 */
class DuplexStream {
public:
  DuplexStream(DuplexStream &&) noexcept            = default;
  DuplexStream &operator=(DuplexStream &&) noexcept = default;
  DuplexStream(const DuplexStream &)                = delete;
  DuplexStream &operator=(const DuplexStream &)     = delete;

  ~DuplexStream() = default;

  /** @brief Read data written by the other side. Blocks when empty. */
  Promise<ssize_t> read(void *buf, size_t len);

  /** @brief Write data readable by the other side. Blocks when full. */
  Promise<ssize_t> write(const void *buf, size_t len);

  /** @brief No-op — duplex data is always "flushed" on write. */
  Promise<void> flush() {
    return xpp::resolve();
  }

  /** @brief Close our write direction. The other side sees EOF on read. */
  void close();

private:
  friend std::pair<DuplexStream, DuplexStream> duplex(size_t size);

  Shared<_::DuplexBuf> m_dup;
  int                  m_idx = 0;

  DuplexStream(Shared<_::DuplexBuf> dup, int idx) : m_dup(std::move(dup)), m_idx(idx) {}
};

/* ── Shared method (no coroutine dep) ───────────────────────────────── */

inline void DuplexStream::close() {
  auto *d = m_dup.get();
  if (!d) return;
  auto *other = &d->side[1 - m_idx];
  if (other->closed) return;
  other->closed = true;
  if (other->read_waiter.is_pending()) {
    auto r = std::move(other->read_waiter);
    r.resolve();
  }
  if (other->write_waiter.is_pending()) {
    auto w = std::move(other->write_waiter);
    w.resolve();
  }
}

/* ── read / write — two implementations ─────────────────────────────── */

#if XPP_HAS_COROUTINES

/* ═══ C++20: coroutine with while + co_await ══════════════════════════ */

inline Promise<ssize_t> DuplexStream::read(void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) co_return static_cast<ssize_t>(-1);
  auto *my = &d->side[m_idx];

  while (my->count == 0) {
    if (my->closed) co_return 0;
    auto pr         = xpp::async<void>();
    my->read_waiter = std::move(pr.second);
    co_await std::move(pr.first);
  }

  size_t avail = my->count;
  size_t n     = len < avail ? len : avail;
  size_t first = my->rpos;
  if (first + n > my->buf.size()) {
    size_t chunk = my->buf.size() - first;
    std::memcpy(buf, my->buf.data() + first, chunk);
    size_t rem = n - chunk;
    std::memcpy(static_cast<uint8_t *>(buf) + chunk, my->buf.data(), rem);
    my->rpos = rem;
  } else {
    std::memcpy(buf, my->buf.data() + first, n);
    my->rpos = first + n;
  }
  my->count -= n;

  if (my->write_waiter.is_pending()) {
    auto w = std::move(my->write_waiter);
    w.resolve();
  }
  co_return static_cast<ssize_t>(n);
}

inline Promise<ssize_t> DuplexStream::write(const void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) co_return static_cast<ssize_t>(-1);
  if (len == 0) co_return 0;

  auto *other = &d->side[1 - m_idx];

  while (other->count + len > other->buf.size()) {
    if (other->closed) co_return static_cast<ssize_t>(-1);
    auto pr             = xpp::async<void>();
    other->write_waiter = std::move(pr.second);
    co_await std::move(pr.first);
  }

  const uint8_t *src = static_cast<const uint8_t *>(buf);
  size_t         pos = other->wpos;
  if (pos + len > other->buf.size()) {
    size_t chunk = other->buf.size() - pos;
    std::memcpy(other->buf.data() + pos, src, chunk);
    size_t rem = len - chunk;
    std::memcpy(other->buf.data(), src + chunk, rem);
    other->wpos = rem;
  } else {
    std::memcpy(other->buf.data() + pos, src, len);
    other->wpos = pos + len;
  }
  other->count += len;

  if (other->read_waiter.is_pending()) {
    auto r = std::move(other->read_waiter);
    r.resolve();
  }
  co_return static_cast<ssize_t>(len);
}

#else // !XPP_HAS_COROUTINES

#if XPP_FIBER

/* ═══ C++11 + fiber: linear while + .await() ═════════════════════════ */

inline Promise<ssize_t> DuplexStream::read(void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
  auto *my = &d->side[m_idx];

  while (my->count == 0) {
    if (my->closed) return xpp::resolve(0);
    auto pr         = xpp::async<void>();
    my->read_waiter = std::move(pr.second);
    pr.first.await();
  }

  size_t avail = my->count;
  size_t n     = len < avail ? len : avail;
  size_t first = my->rpos;
  if (first + n > my->buf.size()) {
    size_t chunk = my->buf.size() - first;
    std::memcpy(buf, my->buf.data() + first, chunk);
    size_t rem = n - chunk;
    std::memcpy(static_cast<uint8_t *>(buf) + chunk, my->buf.data(), rem);
    my->rpos = rem;
  } else {
    std::memcpy(buf, my->buf.data() + first, n);
    my->rpos = first + n;
  }
  my->count -= n;

  if (my->write_waiter.is_pending()) {
    auto w = std::move(my->write_waiter);
    w.resolve();
  }
  return xpp::resolve(static_cast<ssize_t>(n));
}

inline Promise<ssize_t> DuplexStream::write(const void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
  if (len == 0) return xpp::resolve(0);

  auto *other = &d->side[1 - m_idx];

  while (other->count + len > other->buf.size()) {
    if (other->closed) return xpp::resolve(static_cast<ssize_t>(-1));
    auto pr             = xpp::async<void>();
    other->write_waiter = std::move(pr.second);
    pr.first.await();
  }

  const uint8_t *src = static_cast<const uint8_t *>(buf);
  size_t         pos = other->wpos;
  if (pos + len > other->buf.size()) {
    size_t chunk = other->buf.size() - pos;
    std::memcpy(other->buf.data() + pos, src, chunk);
    size_t rem = len - chunk;
    std::memcpy(other->buf.data(), src + chunk, rem);
    other->wpos = rem;
  } else {
    std::memcpy(other->buf.data() + pos, src, len);
    other->wpos = pos + len;
  }
  other->count += len;

  if (other->read_waiter.is_pending()) {
    auto r = std::move(other->read_waiter);
    r.resolve();
  }
  return xpp::resolve(static_cast<ssize_t>(len));
}

#else // !XPP_FIBER

/* ═══ C++11: struct + std::move(*this) recursive .then() chain ════════
 *
 * ReadLoop / WriteLoop hold dup + idx + buf/len (~24 bytes), moved
 * through .then() nodes via std::move(*this). Zero extra heap alloc
 * per IO — only the PromiseNode arena bump.
 * ─────────────────────────────────────────────────────────────────── */

inline Promise<ssize_t> DuplexStream::read(void *buf, size_t len) {
  struct ReadLoop {
    Shared<_::DuplexBuf> dup;
    int                  idx;
    void                *buf;
    size_t               len;

    Promise<ssize_t> operator()() {
      auto *d = dup.get();
      if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
      auto *my = &d->side[idx];

      if (my->count > 0) {
        size_t avail = my->count;
        size_t n     = len < avail ? len : avail;
        size_t first = my->rpos;
        if (first + n > my->buf.size()) {
          size_t chunk = my->buf.size() - first;
          std::memcpy(buf, my->buf.data() + first, chunk);
          size_t rem = n - chunk;
          std::memcpy(static_cast<uint8_t *>(buf) + chunk, my->buf.data(), rem);
          my->rpos = rem;
        } else {
          std::memcpy(buf, my->buf.data() + first, n);
          my->rpos = first + n;
        }
        my->count -= n;

        if (my->write_waiter.is_pending()) {
          auto w = std::move(my->write_waiter);
          w.resolve();
        }
        return xpp::resolve(static_cast<ssize_t>(n));
      }

      if (my->closed) return xpp::resolve(0);

      auto pr         = xpp::async<void>();
      my->read_waiter = std::move(pr.second);
      return std::move(pr.first).then([self = std::move(*this)](Void) mutable { return self(); });
    }
  };

  return ReadLoop{m_dup, m_idx, buf, len}();
}

inline Promise<ssize_t> DuplexStream::write(const void *buf, size_t len) {
  struct WriteLoop {
    Shared<_::DuplexBuf> dup;
    int                  idx;
    const void          *buf;
    size_t               len;

    Promise<ssize_t> operator()() {
      auto *d = dup.get();
      if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
      if (len == 0) return xpp::resolve(0);

      auto *other = &d->side[1 - idx];

      if (other->count + len <= other->buf.size()) {
        const uint8_t *src = static_cast<const uint8_t *>(buf);
        size_t         pos = other->wpos;
        if (pos + len > other->buf.size()) {
          size_t chunk = other->buf.size() - pos;
          std::memcpy(other->buf.data() + pos, src, chunk);
          size_t rem = len - chunk;
          std::memcpy(other->buf.data(), src + chunk, rem);
          other->wpos = rem;
        } else {
          std::memcpy(other->buf.data() + pos, src, len);
          other->wpos = pos + len;
        }
        other->count += len;

        if (other->read_waiter.is_pending()) {
          auto r = std::move(other->read_waiter);
          r.resolve();
        }
        return xpp::resolve(static_cast<ssize_t>(len));
      }

      if (other->closed) return xpp::resolve(static_cast<ssize_t>(-1));

      auto pr             = xpp::async<void>();
      other->write_waiter = std::move(pr.second);
      return std::move(pr.first).then([self = std::move(*this)](Void) mutable { return self(); });
    }
  };

  return WriteLoop{m_dup, m_idx, buf, len}();
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

/* ── Factory (shared — both branches use Shared) ───────────────────── */

inline std::pair<DuplexStream, DuplexStream> duplex(size_t size) {
  auto dup = Shared<_::DuplexBuf>::make(size);
  return {DuplexStream(dup, 0), DuplexStream(std::move(dup), 1)};
}

} // namespace io
} // namespace xpp

#endif // XPP_IO_DUPLEX_H
