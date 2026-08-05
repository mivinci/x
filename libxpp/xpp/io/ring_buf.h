/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ring_buf.h - xpp::io::_::RingBuf: shared ring buffer for pipes.
 *
 * Extracted from simplex.h and duplex.h. Both use the same circular
 * buffer pattern — this header provides the common data + helpers.
 * Waiter management (PromiseResolver read/write waiters) is also
 * included because the wake-reader-on-write / wake-writer-on-read
 * pattern is identical in both pipe types.
 *
 * Internal header — do not include directly.
 */

#ifndef XPP_IO_RING_BUF_H
#define XPP_IO_RING_BUF_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <xpp/promise.h>

namespace xpp {
namespace io {
namespace _ {

/**
 * @brief Circular byte buffer with read/write waiter slots.
 *
 * Pure data — no coroutine dependency. Used internally by simplex()
 * and duplex() pipes. The buffer capacity is fixed at construction.
 *
 * Read/write operations handle wrap-around with two memcpy calls.
 * Callers are responsible for checking availability before calling
 * do_read/do_write — these methods assume the caller has verified
 * that count > 0 (for read) or count < size (for write).
 */
struct RingBuf {
  std::vector<uint8_t>  buf;            // Underlying byte storage.
  size_t                rpos   = 0;     // Next read position (circular).
  size_t                wpos   = 0;     // Next write position (circular).
  size_t                count  = 0;     // Bytes currently buffered.
  bool                  closed = false; // Whether the write end is closed.
  PromiseResolver<void> read_waiter;    // Resolves when data becomes available.
  PromiseResolver<void> write_waiter;   // Resolves when space becomes available.

  explicit RingBuf(size_t size) : buf(size) {}

  /** @brief Read up to @p len bytes into @p dst. Returns bytes copied. */
  size_t do_read(void *dst, size_t len) noexcept;

  /** @brief Write up to @p len bytes from @p src. Returns bytes copied. */
  size_t do_write(const void *src, size_t len) noexcept;

  /** @brief Wake the pending read waiter if any. */
  void wake_reader() noexcept;

  /** @brief Wake the pending write waiter if any. */
  void wake_writer() noexcept;

  /** @brief Wake both waiters (used by close). */
  void wake_all() noexcept;
};

/* ── Inline implementations ─────────────────────────────────────── */

inline size_t RingBuf::do_read(void *dst, size_t len) noexcept {
  size_t n     = len < count ? len : count;
  size_t first = rpos;
  if (first + n > buf.size()) {
    size_t chunk = buf.size() - first;
    std::memcpy(dst, buf.data() + first, chunk);
    size_t rem = n - chunk;
    std::memcpy(static_cast<uint8_t *>(dst) + chunk, buf.data(), rem);
    rpos = rem;
  } else {
    std::memcpy(dst, buf.data() + first, n);
    rpos = first + n;
  }
  count -= n;
  return n;
}

inline size_t RingBuf::do_write(const void *src, size_t len) noexcept {
  const uint8_t *s   = static_cast<const uint8_t *>(src);
  size_t         pos = wpos;
  if (pos + len > buf.size()) {
    size_t chunk = buf.size() - pos;
    std::memcpy(buf.data() + pos, s, chunk);
    size_t rem = len - chunk;
    std::memcpy(buf.data(), s + chunk, rem);
    wpos = rem;
  } else {
    std::memcpy(buf.data() + pos, s, len);
    wpos = pos + len;
  }
  count += len;
  return len;
}

inline void RingBuf::wake_reader() noexcept {
  if (read_waiter.is_pending()) {
    auto r = std::move(read_waiter);
    r.resolve();
  }
}

inline void RingBuf::wake_writer() noexcept {
  if (write_waiter.is_pending()) {
    auto w = std::move(write_waiter);
    w.resolve();
  }
}

inline void RingBuf::wake_all() noexcept {
  wake_reader();
  wake_writer();
}

} // namespace _
} // namespace io
} // namespace xpp

#endif // XPP_IO_RING_BUF_H
