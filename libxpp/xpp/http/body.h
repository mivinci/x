/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * body.h - xpp::http::Body: HTTP message body.
 *
 * Three states: Empty, Once (a single Bytes), Channel (a stream of Bytes
 * fed by an mpsc::Receiver).
 *
 * Satisfies the AsyncReader concept: read(void*, size_t) → Promise<ssize_t>.
 * Empty returns 0 (EOF) immediately. Once slices the stored bytes.
 * Channel drains buffered chunks first, then awaits the receiver.
 *   - If a chunk is larger than the requested buffer, only len bytes are
 *     copied out and the remainder is kept in m_pending for the next read.
 *   - channel close (recv() returns None) is treated as EOF.
 *
 * Convenience aggregators:
 *   bytes() → Promise<Result<Bytes>>   — io::read_all + Bytes::from
 *   text()  → Promise<Result<String>> — bytes() + Bytes::to_string
 *
 * Mirrors hyper::Body. C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_BODY_H
#define XPP_HTTP_BODY_H

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/bytes.h>
#include <xpp/enum.h>
#include <xpp/http/error.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/string.h>
#include <xpp/sync/mpsc.h>
#include <xpp/vec.h>

// For io::read_all and io::copy — body.h needs them to implement bytes().
// They are templates and will be instantiated with Body as R.
#include <xpp/io/utils.h>

namespace xpp {
namespace http {

/**
 * @brief HTTP message body. Empty / Once / Channel.
 *
 * @code
 *   // From a static buffer
 *   Body b = Body::from(Bytes::from("hello"));
 *
 *   // From a streaming source (e.g. xHttpClient on_data callback)
 *   auto [tx, rx] = sync::mpsc::channel<Bytes>(64);
 *   Body  b = Body::from_channel(std::move(rx));
 *   // push chunks from another fiber:
 *   //   tx.try_send(Bytes::copy(p, n));
 *   //   tx.close();  // signals EOF
 *
 *   // Read it
 *   char  buf[1024];
 *   ssize_t n = b.read(buf, sizeof(buf)).await();
 *   // n == 0 means EOF
 *
 *   // Or consume all at once
 *   Bytes bytes = b.bytes().await().unwrap();
 *   String text = b.text().await().unwrap();
 * @endcode
 */
class Body {
public:
  /** @brief Construct an empty body (EOF on first read). */
  Body()                        = default; // m_storage = None
  Body(const Body &)            = delete;
  Body &operator=(const Body &) = delete;

  /* ── Factories ─────────────────────────────────────────────────── */

  /** @brief Empty body. read() returns 0 (EOF) immediately. */
  static Body empty() {
    return Body();
  }

  /** @brief A body holding a single Bytes buffer. Read drains it. */
  static Body from(Bytes bytes) {
    if (bytes.size() == 0) return Body();
    Body b;
    b.m_storage = xpp::some(Enum<Bytes, sync::mpsc::Receiver<Bytes>>(std::move(bytes)));
    return b;
  }

  /** @brief Convenience: take ownership of a Vec<uint8_t>. */
  static Body from(Vec<uint8_t> bytes) {
    return from(Bytes::from(std::move(bytes)));
  }

  /** @brief Convenience: from a UTF-8 String. */
  static Body from(String text) {
    return from(Bytes::from(std::move(text)));
  }

  /** @brief Convenience: from a C string (no trailing NUL). */
  static Body from(const char *text) {
    return from(Bytes::from(text));
  }

  /**
   * @brief A streaming body fed by an mpsc::Receiver<Bytes>.
   *
   * The receiver returns Bytes chunks. When the channel closes
   * (recv() returns None), read() returns 0 (EOF).
   */
  static Body from_channel(sync::mpsc::Receiver<Bytes> rx) {
    Body b;
    b.m_storage = xpp::some(Enum<Bytes, sync::mpsc::Receiver<Bytes>>(std::move(rx)));
    return b;
  }

  ~Body() = default; // Enum + Option handle destruction

  /* ── Move constructor / assignment ──────────────────────────────── */

  Body(Body &&) noexcept            = default;
  Body &operator=(Body &&) noexcept = default;

  /* ── AsyncReader concept ──────────────────────────────────────── */

  /**
   * @brief Read up to @p len bytes into @p buf.
   *
   * Returns a Promise<ssize_t>:
   *   - n > 0: n bytes were read into @p buf.
   *   - n == 0: EOF (no more data).
   *   - n < 0: error (reserved — current implementation never returns this).
   *
   * The promise may be Pending if the Channel has no buffered chunk yet;
   * the calling fiber/coroutine is suspended until a chunk arrives or
   * the sender closes the channel.
   */
  Promise<ssize_t> read(void *buf, size_t len);

  /* ── Convenience aggregators ─────────────────────────────────── */

  /**
   * @brief Consume the entire body into a single Bytes.
   *
   * Uses io::read_all internally. The current implementation cannot fail
   * (read() never returns negative), so the result is always Ok. The
   * Result<Bytes> return type is reserved for future I/O error surfacing
   * (e.g. connection reset mid-stream).
   *
   * After this call, the Body is consumed (reads will return 0).
   */
  Promise<http::Result<Bytes>> bytes() {
    return io::read_all(*this).then(
      [](Vec<uint8_t> v) { return http::Result<Bytes>(xpp::ok, Bytes::from(std::move(v))); });
  }

  /**
   * @brief Consume the entire body and decode as UTF-8.
   *
   * Returns Err(Error{Kind::Body, ...}) if the body is not valid UTF-8.
   */
  Promise<http::Result<String>> text() {
    return bytes().then([](http::Result<Bytes> r) -> http::Result<String> {
      return r.and_then([](Bytes b) {
        return b.to_string().map_err([](Utf8Error) {
          return Error(Error::Kind::Body, String::from_utf8("invalid UTF-8 in body").unwrap());
        });
      });
    });
  }

  /* ── Observers ─────────────────────────────────────────────────── */

  /** @brief True if the body has no data (Empty kind or exhausted). */
  bool is_empty() const noexcept {
    return m_storage.is_none();
  }

  /** @brief True if the body is fed by a streaming channel. */
  bool is_channel() const noexcept {
    return m_storage.is_some() && m_storage.unwrap().template is<sync::mpsc::Receiver<Bytes>>();
  }

private:
  using Storage = Enum<Bytes, sync::mpsc::Receiver<Bytes>>;

  // None = Empty; Some(Once|Channel) holds the active source.
  Option<Storage> m_storage;

  // In Channel mode: leftover bytes from a chunk larger than the read
  // buffer. Drained first on the next read() before consulting m_rx.
  Bytes m_pending;
};

/* ── Body::read() — three implementations ─────────────────────────── */

#if XPP_HAS_COROUTINES

/* ═══ C++20 coroutine version ═══════════════════════════════════════ */

inline Promise<ssize_t> Body::read(void *buf, size_t len) {
  if (len == 0) co_return 0;

  if (m_storage.is_none()) co_return 0;

  Storage &s = m_storage.unwrap();
  if (s.is<Bytes>()) {
    Bytes &once = s.get<Bytes>();
    size_t n    = once.size() < len ? once.size() : len;
    if (n == 0) {
      m_storage = none;
      co_return 0;
    }
    std::memcpy(buf, once.data(), n);
    once = once.slice_from(n);
    if (once.size() == 0) m_storage = none;
    co_return static_cast<ssize_t>(n);
  }

  // Channel
  auto &rx = s.get<sync::mpsc::Receiver<Bytes>>();
  if (m_pending.size() > 0) {
    size_t n = m_pending.size() < len ? m_pending.size() : len;
    std::memcpy(buf, m_pending.data(), n);
    m_pending = m_pending.slice_from(n);
    co_return static_cast<ssize_t>(n);
  }
  auto opt = co_await rx.recv();
  if (opt.is_none()) co_return 0; // channel closed = EOF
  Bytes chunk = opt.unwrap();
  if (chunk.size() == 0) co_return 0; // defensive: empty chunk = EOF
  size_t n = chunk.size() < len ? chunk.size() : len;
  std::memcpy(buf, chunk.data(), n);
  if (n < chunk.size()) m_pending = chunk.slice_from(n);
  co_return static_cast<ssize_t>(n);
}

#else // !XPP_HAS_COROUTINES

#if XPP_FIBER

/* ═══ C++11 + fiber: linear .await() ═══════════════════════════════ */

inline Promise<ssize_t> Body::read(void *buf, size_t len) {
  if (len == 0) return xpp::resolve(static_cast<ssize_t>(0));

  if (m_storage.is_none()) return xpp::resolve(static_cast<ssize_t>(0));

  Storage &s = m_storage.unwrap();
  if (s.is<Bytes>()) {
    Bytes &once = s.get<Bytes>();
    size_t n    = once.size() < len ? once.size() : len;
    if (n == 0) {
      m_storage = none;
      return xpp::resolve(static_cast<ssize_t>(0));
    }
    std::memcpy(buf, once.data(), n);
    once = once.slice_from(n);
    if (once.size() == 0) m_storage = none;
    return xpp::resolve(static_cast<ssize_t>(n));
  }

  // Channel
  auto &rx = s.get<sync::mpsc::Receiver<Bytes>>();
  if (m_pending.size() > 0) {
    size_t n = m_pending.size() < len ? m_pending.size() : len;
    std::memcpy(buf, m_pending.data(), n);
    m_pending = m_pending.slice_from(n);
    return xpp::resolve(static_cast<ssize_t>(n));
  }
  auto opt = rx.recv().await();
  if (opt.is_none()) return xpp::resolve(static_cast<ssize_t>(0));
  Bytes chunk = opt.unwrap();
  if (chunk.size() == 0) return xpp::resolve(static_cast<ssize_t>(0));
  size_t n = chunk.size() < len ? chunk.size() : len;
  std::memcpy(buf, chunk.data(), n);
  if (n < chunk.size()) m_pending = chunk.slice_from(n);
  return xpp::resolve(static_cast<ssize_t>(n));
}

#else // !XPP_FIBER

/* ═══ C++11 only: .then() chain for Channel ═════════════════════════
 *
 * Empty and Once are synchronous — they return immediately-resolved
 * promises. Channel's recv() returns Promise<Option<Bytes>>, so we
 * chain a .then() to handle the result.
 *
 * The "drain pending" path is synchronous; only the "await receiver"
 * path uses .then(). This keeps the common case (chunks are usually
 * larger than the read buffer, so we mostly drain m_pending) fast.
 */

inline Promise<ssize_t> Body::read(void *buf, size_t len) {
  if (len == 0) return xpp::resolve(static_cast<ssize_t>(0));

  if (m_storage.is_none()) return xpp::resolve(static_cast<ssize_t>(0));

  Storage &s = m_storage.unwrap();
  if (s.is<Bytes>()) {
    Bytes &once = s.get<Bytes>();
    size_t n    = once.size() < len ? once.size() : len;
    if (n == 0) {
      m_storage = none;
      return xpp::resolve(static_cast<ssize_t>(0));
    }
    std::memcpy(buf, once.data(), n);
    once = once.slice_from(n);
    if (once.size() == 0) m_storage = none;
    return xpp::resolve(static_cast<ssize_t>(n));
  }

  // Channel
  auto &rx = s.get<sync::mpsc::Receiver<Bytes>>();
  // Drain pending first — synchronous.
  if (m_pending.size() > 0) {
    size_t n = m_pending.size() < len ? m_pending.size() : len;
    std::memcpy(buf, m_pending.data(), n);
    m_pending = m_pending.slice_from(n);
    return xpp::resolve(static_cast<ssize_t>(n));
  }
  // Need a chunk from the channel. Capture buf + this by pointer —
  // the caller's fiber/stack holds them alive while awaiting.
  // Body itself is captured via `this` (the Promise returned is
  // awaited by the same caller that owns the Body).
  return rx.recv().then([this, buf, len](Option<Bytes> opt) {
    if (opt.is_none()) return xpp::resolve(static_cast<ssize_t>(0));
    Bytes chunk = opt.unwrap();
    if (chunk.size() == 0) return xpp::resolve(static_cast<ssize_t>(0));
    size_t n = chunk.size() < len ? chunk.size() : len;
    std::memcpy(buf, chunk.data(), n);
    if (n < chunk.size()) m_pending = chunk.slice_from(n);
    return xpp::resolve(static_cast<ssize_t>(n));
  });
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_BODY_H
