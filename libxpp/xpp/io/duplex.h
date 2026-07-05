/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * duplex.h - xpp::io::duplex(): in-memory bidirectional pipe.
 *
 * Creates a reader/writer pair backed by a shared ring buffer. Like
 * Go's io.Pipe or tokio's DuplexStream — useful for testing I/O
 * utilities without real connections.
 *
 * Both halves are coroutine-based (C++20). Read blocks when buffer
 * is empty; write blocks when buffer is full. Close the writer to
 * signal EOF to the reader.
 *
 * Single-threaded — no atomics or mutex.
 *
 * Usage:
 * @code
 *   auto [reader, writer] = xpp::io::duplex(4096);
 *
 *   // Read + write concurrently
 *   char buf[16];
 *   auto rp = reader.read(buf, 16);
 *   auto wp = writer.write("hello", 5);
 *   xpp::all(std::move(rp), std::move(wp)).wait();
 * @endcode
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

#if XPP_HAS_COROUTINES

namespace xpp {
namespace io {

/* ── Shared state ─────────────────────────────────────────────────── */

namespace _ {

struct DuplexBuf {
  std::vector<uint8_t>  buf;
  size_t                rpos   = 0;
  size_t                wpos   = 0;
  size_t                count  = 0;
  bool                  closed = false;
  PromiseResolver<void> read_waiter;
  PromiseResolver<void> write_waiter;

  explicit DuplexBuf(size_t size) : buf(size) {}
};

} // namespace _

class WriteHalf; // forward

/* ── ReadHalf ──────────────────────────────────────────────────────── */

/**
 * @brief Reader half of a duplex pipe. Satisfies `AsyncReader`.
 *
 * Reads from the shared ring buffer. Suspends (co_await) when the
 * buffer is empty. Returns 0 (EOF) when the writer closes.
 */
class ReadHalf {
public:
  ReadHalf() = default;

  ReadHalf(ReadHalf &&) noexcept            = default;
  ReadHalf &operator=(ReadHalf &&) noexcept = default;
  ReadHalf(const ReadHalf &)                = delete;
  ReadHalf &operator=(const ReadHalf &)     = delete;

  ~ReadHalf() = default;

  /** @brief Read from the pipe. Suspends when empty, returns 0 at EOF. */
  Promise<ssize_t> read(void *buf, size_t len) {
    auto *d = m_dup.get();
    if (!d) co_return static_cast<ssize_t>(-1);

    // Wait for data
    while (d->count == 0) {
      if (d->closed) co_return 0;

      // Suspend — will be woken by WriteHalf::write or WriteHalf::close
      auto pr        = xpp::async<void>();
      d->read_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }

    // Copy data from ring buffer
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

    // Wake blocked writer
    if (d->write_waiter.is_pending()) {
      auto w = std::move(d->write_waiter);
      w.resolve();
    }

    co_return static_cast<ssize_t>(n);
  }

private:
  friend class WriteHalf;
  friend std::pair<ReadHalf, WriteHalf> duplex(size_t size);

  Arc<_::DuplexBuf> m_dup;

  explicit ReadHalf(Arc<_::DuplexBuf> dup) : m_dup(std::move(dup)) {}
};

/* ── WriteHalf ─────────────────────────────────────────────────────── */

/**
 * @brief Writer half of a duplex pipe. Satisfies `AsyncWriter`.
 *
 * Writes to the shared ring buffer. Suspends (co_await) when the
 * buffer is full. Call `close()` to signal EOF to the reader.
 *
 * flush() is a no-op (data goes directly to buffer, no inner writer).
 */
class WriteHalf {
public:
  WriteHalf() = default;

  WriteHalf(WriteHalf &&) noexcept            = default;
  WriteHalf &operator=(WriteHalf &&) noexcept = default;
  WriteHalf(const WriteHalf &)                = delete;
  WriteHalf &operator=(const WriteHalf &)     = delete;

  ~WriteHalf() = default;

  /** @brief Write to the pipe. Suspends when buffer is full. */
  Promise<ssize_t> write(const void *buf, size_t len) {
    auto *d = m_dup.get();
    if (!d) co_return static_cast<ssize_t>(-1);

    if (len == 0) co_return 0;

    // Buffer is full — suspend and wait for ReadHalf to drain
    while (d->count + len > d->buf.size()) {
      if (d->closed) co_return static_cast<ssize_t>(-1);

      auto pr         = xpp::async<void>();
      d->write_waiter = std::move(pr.second);
      co_await std::move(pr.first);
    }

    // Copy data into ring buffer
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

    // Wake blocked reader
    if (d->read_waiter.is_pending()) {
      auto r = std::move(d->read_waiter);
      r.resolve();
    }

    co_return static_cast<ssize_t>(len);
  }

  /** @brief No-op for duplex (data flows directly, no inner buffer). */
  Promise<void> flush() {
    co_return;
  }

  /** @brief Close the pipe. Wakes pending readers with EOF. */
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
  friend class ReadHalf;
  friend std::pair<ReadHalf, WriteHalf> duplex(size_t size);

  Arc<_::DuplexBuf> m_dup;

  explicit WriteHalf(Arc<_::DuplexBuf> dup) : m_dup(std::move(dup)) {}
};

/* ── Factory ──────────────────────────────────────────────────────── */

/**
 * @brief Create a duplex pipe (in-memory bidirectional reader/writer pair).
 *
 * The two halves share a ring buffer of `size` bytes. ReadHalf satisfies
 * `AsyncReader`; WriteHalf satisfies `AsyncWriter`. Useful for testing
 * I/O utilities without real TCP connections.
 *
 * @param size Size of the shared ring buffer in bytes.
 * @return A pair of {ReadHalf, WriteHalf}.
 */
inline std::pair<ReadHalf, WriteHalf> duplex(size_t size) {
  auto dup = Arc<_::DuplexBuf>::make(size);
  return {ReadHalf(dup), WriteHalf(std::move(dup))};
}

} // namespace io
} // namespace xpp

#endif // XPP_HAS_COROUTINES

#endif // XPP_IO_DUPLEX_H
