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
#include <functional>
#include <utility>

#include <xpp/arc.h>
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
  Body() : m_xfer_error(Arc<Option<Error>>::make()) {} // m_storage = None
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
   *
   * @param on_drain Optional callback invoked each time a chunk is
   *        successfully pulled from the channel. Used for backpressure:
   *        the producer (e.g. xHttpClient's on_data) can pause when the
   *        channel is full and resume from this callback once the
   *        consumer drains a slot.
   * @param xfer_error Optional shared error flag written by the transfer
   *        owner (e.g. xHttpClient's on_done) when the connection fails
   *        mid-body; the Body reports it as a read error instead of a
   *        clean EOF.
   */
  static Body from_channel(sync::mpsc::Receiver<Bytes> rx,
                           std::function<void()>       on_drain   = std::function<void()>(),
                           Arc<Option<Error>>          xfer_error = Arc<Option<Error>>::make()) {
    Body b;
    b.m_on_drain   = std::move(on_drain);
    b.m_xfer_error = std::move(xfer_error);
    b.m_storage    = xpp::some(Enum<Bytes, sync::mpsc::Receiver<Bytes>>(std::move(rx)));
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
   *
   * @warning The Body must outlive the returned Promise. Reading from a
   * temporary (`resp.into_body().read(buf, n).await()`) is undefined
   * behavior — unlike bytes()/text(), read() does not extend the Body's
   * lifetime. Keep the Body in a named variable while awaiting.
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
    // Move *this into an Arc<Body> so the reader stays alive across the
    // async read loop. `io::read_all(*this)` only stores a reference; if
    // the Body is a temporary (e.g. `Response::bytes()` → `into_body().bytes()`)
    // it would be destroyed before the promise resolves. Holding `self`
    // in the .then() capture extends its lifetime to the full Promise chain.
    Arc<Body> self = Arc<Body>::make(std::move(*this));
    return io::read_all(*self).then([self](Vec<uint8_t> v) {
      if (self->m_error.is_some()) {
        return http::Result<Bytes>(xpp::err, self->m_error.unwrap());
      }
      return http::Result<Bytes>(xpp::ok, Bytes::from(std::move(v)));
    });
  }

  /**
   * @brief Consume the entire body and decode as UTF-8.
   *
   * Returns Err(Error{Kind::Body, ...}) if the body is not valid UTF-8.
   */
  Promise<http::Result<String>> text() {
    // bytes() moves *this into a shared_ptr internally; the returned
    // Promise chain holds that shared_ptr, keeping the Body alive until
    // the chain completes.
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

  /**
   * @brief Take the stored bytes of a Once body, leaving the Body empty.
   *
   * Returns an empty Bytes for an Empty body, and an empty Bytes for a
   * Channel body (which must be drained asynchronously via read()/bytes()).
   * Used by Client::send to feed libcurl's synchronous upload callback.
   */
  Bytes into_once_bytes() noexcept {
    if (m_storage.is_none()) return Bytes();
    Storage &s = m_storage.unwrap();
    if (!s.is<Bytes>()) return Bytes();
    Bytes b   = std::move(s.get<Bytes>());
    m_storage = none;
    return b;
  }

private:
  using Storage = Enum<Bytes, sync::mpsc::Receiver<Bytes>>;

  // None = Empty; Some(Once|Channel) holds the active source.
  Option<Storage> m_storage;

  // Backpressure hook: fired after each successful recv (see from_channel).
  std::function<void()> m_on_drain;

  // Transfer-error flag (shared via Arc, see from_channel). Written by the transfer
  // owner; read at channel EOF. When set, read() returns -1 instead of 0.
  Arc<Option<Error>> m_xfer_error;
  Option<Error>      m_error; // surfaced to bytes()/text()

  // In Channel mode: leftover bytes from a chunk larger than the read
  // buffer. Drained first on the next read() before consulting m_rx.
  Bytes m_pending;

  /**
   * @brief Channel EOF handling: 0 for a clean close, -1 if the shared
   * transfer-error flag was set (recording the error for bytes()/text()).
   */
  ssize_t channel_eof() noexcept {
    if (m_xfer_error.get() && m_xfer_error->is_some()) {
      m_error = xpp::some(m_xfer_error->unwrap());
      return -1;
    }
    return 0;
  }

  /* ── Synchronous helpers shared by all three read() implementations.
   *    The async-await mechanism (co_await / .await() / .then()) is the
   *    only thing that differs between C++20, fiber, and C++11 builds. */

  /// Copy up to @p len bytes from a Once body, advancing its cursor.
  ssize_t read_once(Bytes &once, void *buf, size_t len) noexcept {
    size_t n = once.size() < len ? once.size() : len;
    if (n == 0) {
      m_storage = none;
      return 0;
    }
    std::memcpy(buf, once.data(), n);
    once = once.slice_from(n);
    if (once.size() == 0) m_storage = none;
    return static_cast<ssize_t>(n);
  }

  /// Sentinel: drain_pending found no leftover bytes.
  static constexpr ssize_t kNoPending = -2;

  /// Drain leftover bytes from a chunk larger than the read buffer.
  /// Returns bytes copied, or kNoPending if there was nothing pending.
  ssize_t drain_pending(void *buf, size_t len) noexcept {
    if (m_pending.size() == 0) return kNoPending;
    size_t n = m_pending.size() < len ? m_pending.size() : len;
    std::memcpy(buf, m_pending.data(), n);
    m_pending = m_pending.slice_from(n);
    return static_cast<ssize_t>(n);
  }

  /// Copy up to @p len bytes from a received chunk, stashing the rest.
  /// Fires the backpressure resume hook (a channel slot was freed).
  ssize_t take_chunk(const Bytes &chunk, void *buf, size_t len) noexcept {
    if (chunk.size() == 0) return 0; // defensive: empty chunk = EOF
    if (m_on_drain) m_on_drain();    // a slot freed — producer may resume
    size_t n = chunk.size() < len ? chunk.size() : len;
    std::memcpy(buf, chunk.data(), n);
    if (n < chunk.size()) m_pending = chunk.slice_from(n);
    return static_cast<ssize_t>(n);
  }
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
    co_return read_once(once, buf, len);
  }

  // Channel
  auto   &rx = s.get<sync::mpsc::Receiver<Bytes>>();
  ssize_t n  = drain_pending(buf, len);
  if (n != kNoPending) co_return n;
  auto opt = co_await rx.recv();
  if (opt.is_none()) co_return channel_eof(); // closed = EOF or error
  co_return take_chunk(opt.unwrap(), buf, len);
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
    return xpp::resolve(read_once(once, buf, len));
  }

  // Channel
  auto   &rx = s.get<sync::mpsc::Receiver<Bytes>>();
  ssize_t n  = drain_pending(buf, len);
  if (n != kNoPending) return xpp::resolve(n);
  auto opt = rx.recv().await();
  if (opt.is_none()) return xpp::resolve(channel_eof()); // closed = EOF or error
  return xpp::resolve(take_chunk(opt.unwrap(), buf, len));
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
    return xpp::resolve(read_once(once, buf, len));
  }

  // Channel. Drain pending first — synchronous; only the "await receiver"
  // path uses .then(). Capture buf + this by pointer — the caller's
  // fiber/stack holds them alive while awaiting.
  auto   &rx = s.get<sync::mpsc::Receiver<Bytes>>();
  ssize_t n  = drain_pending(buf, len);
  if (n != kNoPending) return xpp::resolve(n);
  return rx.recv().then([this, buf, len](Option<Bytes> opt) {
    if (opt.is_none()) return xpp::resolve(channel_eof()); // closed = EOF or error
    return xpp::resolve(take_chunk(opt.unwrap(), buf, len));
  });
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_BODY_H
