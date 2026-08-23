/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * duplex.h - xpp::io::duplex(): in-memory bidirectional pipe.
 *
 * Returns a pair of DuplexStreams connected by two internal RingBuf
 * instances. Each half satisfies both AsyncReader and AsyncWriter —
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
#include <utility>

#include <xpp/arc.h>
#include <xpp/io/ring_buf.h>
#include <xpp/promise.h>

namespace xpp {
namespace io {

/* ── DuplexBuf ─────────────────────────────────────────────────────── */

namespace _ {

/** @brief Two RingBuf instances, one per direction. */
struct DuplexBuf {
  RingBuf buf[2];

  explicit DuplexBuf(size_t size) : buf{RingBuf(size), RingBuf(size)} {}
};

} // namespace _

/* ── Class declaration ─────────────────────────────────────────────── */

/**
 * @brief One half of a duplex pipe. Satisfies AsyncReader and AsyncWriter.
 *
 * Writing to this side makes data readable on the other side, and
 * vice versa — like a connected socket pair. Each side has its own
 * RingBuf for incoming data.
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

  Arc<_::DuplexBuf> m_dup;
  int               m_idx = 0;

  DuplexStream(Arc<_::DuplexBuf> dup, int idx) : m_dup(std::move(dup)), m_idx(idx) {}

  /** My inbound buffer (read side). */
  _::RingBuf &my_buf() {
    return m_dup->buf[m_idx];
  }
  /** The other side's inbound buffer (my write target). */
  _::RingBuf &other_buf() {
    return m_dup->buf[1 - m_idx];
  }
};

/* ── Shared method (no coroutine dep) ───────────────────────────────── */

inline void DuplexStream::close() {
  auto *d = m_dup.get();
  if (!d) return;
  auto &other = other_buf();
  if (other.closed) return;
  other.closed = true;
  other.wake_all();
}

/* ── read / write — two implementations ─────────────────────────────── */

#if XPP_HAS_COROUTINES

/* ═══ C++20: coroutine with while + co_await ══════════════════════════ */

inline Promise<ssize_t> DuplexStream::read(void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) co_return static_cast<ssize_t>(-1);
  auto &my = my_buf();

  while (my.count == 0) {
    if (my.closed) co_return 0;
    auto pr        = xpp::async<void>();
    my.read_waiter = std::move(pr.second);
    co_await std::move(pr.first);
  }

  size_t n = my.do_read(buf, len);
  my.wake_writer();
  co_return static_cast<ssize_t>(n);
}

inline Promise<ssize_t> DuplexStream::write(const void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) co_return static_cast<ssize_t>(-1);
  if (len == 0) co_return 0;

  auto &other = other_buf();

  while (other.count + len > other.buf.size()) {
    if (other.closed) co_return static_cast<ssize_t>(-1);
    auto pr            = xpp::async<void>();
    other.write_waiter = std::move(pr.second);
    co_await std::move(pr.first);
  }

  other.do_write(buf, len);
  other.wake_reader();
  co_return static_cast<ssize_t>(len);
}

#else // !XPP_HAS_COROUTINES

#if XPP_FIBER

/* ═══ C++11 + fiber: linear while + .await() ═════════════════════════ */

inline Promise<ssize_t> DuplexStream::read(void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
  auto &my = my_buf();

  while (my.count == 0) {
    if (my.closed) return xpp::resolve(0);
    auto pr        = xpp::async<void>();
    my.read_waiter = std::move(pr.second);
    pr.first.await();
  }

  size_t n = my.do_read(buf, len);
  my.wake_writer();
  return xpp::resolve(static_cast<ssize_t>(n));
}

inline Promise<ssize_t> DuplexStream::write(const void *buf, size_t len) {
  auto *d = m_dup.get();
  if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
  if (len == 0) return xpp::resolve(0);

  auto &other = other_buf();

  while (other.count + len > other.buf.size()) {
    if (other.closed) return xpp::resolve(static_cast<ssize_t>(-1));
    auto pr            = xpp::async<void>();
    other.write_waiter = std::move(pr.second);
    pr.first.await();
  }

  other.do_write(buf, len);
  other.wake_reader();
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
    Arc<_::DuplexBuf> dup;
    int               idx;
    void             *buf;
    size_t            len;

    Promise<ssize_t> operator()() {
      auto *d = dup.get();
      if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
      auto &my = d->buf[idx];

      if (my.count > 0) {
        size_t n = my.do_read(buf, len);
        my.wake_writer();
        return xpp::resolve(static_cast<ssize_t>(n));
      }

      if (my.closed) return xpp::resolve(0);

      auto pr        = xpp::async<void>();
      my.read_waiter = std::move(pr.second);
      return std::move(pr.first).then([self = std::move(*this)](Void) mutable { return self(); });
    }
  };

  return ReadLoop{m_dup, m_idx, buf, len}();
}

inline Promise<ssize_t> DuplexStream::write(const void *buf, size_t len) {
  struct WriteLoop {
    Arc<_::DuplexBuf> dup;
    int               idx;
    const void       *buf;
    size_t            len;

    Promise<ssize_t> operator()() {
      auto *d = dup.get();
      if (!d) return xpp::resolve(static_cast<ssize_t>(-1));
      if (len == 0) return xpp::resolve(0);

      auto &other = d->buf[1 - idx];

      if (other.count + len <= other.buf.size()) {
        other.do_write(buf, len);
        other.wake_reader();
        return xpp::resolve(static_cast<ssize_t>(len));
      }

      if (other.closed) return xpp::resolve(static_cast<ssize_t>(-1));

      auto pr            = xpp::async<void>();
      other.write_waiter = std::move(pr.second);
      return std::move(pr.first).then([self = std::move(*this)](Void) mutable { return self(); });
    }
  };

  return WriteLoop{m_dup, m_idx, buf, len}();
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

/* ── Factory (shared — both branches use Shared) ───────────────────── */

inline std::pair<DuplexStream, DuplexStream> duplex(size_t size) {
  auto dup = Arc<_::DuplexBuf>::make(size);
  return {DuplexStream(dup, 0), DuplexStream(std::move(dup), 1)};
}

} // namespace io
} // namespace xpp

#endif // XPP_IO_DUPLEX_H
