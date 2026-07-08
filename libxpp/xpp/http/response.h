/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.

 *
 * response.h - xpp::http::Response: HTTP response with async body.
 *
 * Resolved at header time. Body data flows through a single mpsc
 * channel (populated by on_data, closed by on_done):
 *   - chunk()  — read the next chunk directly from the channel.
 *   - text() / bytes() — drain the channel into a single buffer.
 *
 * Mutually exclusive: once you call chunk(), buffered access is
 * unavailable, and vice versa.
 */

#ifndef XPP_HTTP_RESPONSE_H
#define XPP_HTTP_RESPONSE_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <xpp/bytes/bytes.h>
#include <xpp/bytes/bytes_mut.h>
#include <xpp/compiler.h>
#include <xpp/http/error.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/shared.h>
#include <xpp/sync/mpsc.h>

namespace xpp {
namespace http {

class Response {
public:
  Response() = default;

  // ── Headers (available immediately) ─────────────────────────────

  int status() const { return m_status; }

  Option<std::string> header(const std::string &name) const {
    auto it = m_headers.find(name);
    if (it != m_headers.end()) return Option<std::string>(it->second);
    return none;
  }

  // ── Buffered body access ────────────────────────────────────────

  /// Async: wait for the full body, decode as UTF-8.
  Promise<Result<std::string>> text() {
    return bytes().then([](Result<bytes::Bytes> r) -> Result<std::string> {
      if (r.is_err()) return xpp::err(r.unwrap_err());
      return xpp::ok(r.unwrap_unchecked().to_string());
    });
  }

  /// Async: wait for the full body, return raw bytes.
  Promise<Result<bytes::Bytes>> bytes() {
    XPP_ASSERT(m_access != Access::Streaming,
               "bytes() after chunk() — body already consumed as stream");
    m_access = Access::Buffered;
    XPP_ASSERT(m_body_rx.is_some(), "bytes() on response without body channel");
    return drain_into_bytes(std::move(m_body_rx).unwrap());
  }

  // ── Streaming body access ───────────────────────────────────────

  /// Return the next body chunk.  None means the stream ended.
  ///
  /// Each call blocks until a chunk arrives.  Must be called in a
  /// loop to consume the full body.  Mutually exclusive with text()
  /// and bytes().
  Promise<Option<bytes::Bytes>> chunk() {
    XPP_ASSERT(m_access != Access::Buffered,
               "chunk() after bytes()/text() — body already consumed as buffered");
    m_access = Access::Streaming;
    XPP_ASSERT(m_body_rx.is_some(), "chunk() on a response without body channel");
    return std::move(m_body_rx).unwrap().recv();
  }

  // ── Internal ───────────────────────────────────────────────────

  void set_status(int s) { m_status = s; }

  /// Set the body channel (single source of truth for both access patterns).
  void set_body_channel(sync::mpsc::Receiver<bytes::Bytes> rx) {
    m_body_rx = Option<sync::mpsc::Receiver<bytes::Bytes>>(std::move(rx));
  }

private:
  enum class Access { None, Buffered, Streaming };

  /// Drain the body channel into a single buffered Bytes.
  static Promise<Result<bytes::Bytes>> drain_into_bytes(
    sync::mpsc::Receiver<bytes::Bytes> rx);

  int                                         m_status  = 0;
  Access                                      m_access  = Access::None;
  std::multimap<std::string, std::string>     m_headers;
  Option<sync::mpsc::Receiver<bytes::Bytes>>  m_body_rx;
};

// ═════════════════════════════════════════════════════════════════════
// drain_into_bytes — channel → single Bytes
//
// Three implementations, selected at compile time:
//   1. C++20 coroutine (co_await)       — compiler-generated state machine
//   2. XPP_FIBER (.await())             — linear code, fiber-suspend
//   3. C++11 .then() chain              — struct + std::move(*this) recursion
// ═════════════════════════════════════════════════════════════════════

#if XPP_HAS_COROUTINES

// C++20 coroutine — simplest, compiler does the state machine.
inline Promise<Result<bytes::Bytes>> Response::drain_into_bytes(
    sync::mpsc::Receiver<bytes::Bytes> rx) {
  bytes::BytesMut buf;
  while (true) {
    auto chunk = co_await rx.recv();
    if (chunk.is_none()) break;
    auto &&b = chunk.unwrap();
    buf.put(b.data(), b.size());
  }
  co_return Result<bytes::Bytes>(xpp::ok, buf.freeze());
}

#elif XPP_FIBER

// Fiber — linear code with .await() that suspends the fiber.
inline Promise<Result<bytes::Bytes>> Response::drain_into_bytes(
    sync::mpsc::Receiver<bytes::Bytes> rx) {
  bytes::BytesMut buf;
  while (true) {
    auto chunk = rx.recv().await();
    if (chunk.is_none()) break;
    auto &&b = chunk.unwrap();
    buf.put(b.data(), b.size());
  }
  return xpp::resolve(Result<bytes::Bytes>(xpp::ok, buf.freeze()));
}

#else

// C++11 — struct + std::move(*this) tail-recursive .then() chain.
// State is Shared (one heap alloc) for the buffer and receiver.
inline Promise<Result<bytes::Bytes>> Response::drain_into_bytes(
    sync::mpsc::Receiver<bytes::Bytes> rx) {
  struct State {
    sync::mpsc::Receiver<bytes::Bytes> rx;
    bytes::BytesMut                    buf;
    explicit State(sync::mpsc::Receiver<bytes::Bytes> r) : rx(std::move(r)) {}
  };
  auto state = Shared<State>::make(std::move(rx));

  struct DrainLoop {
    Shared<State> state;
    Promise<Result<bytes::Bytes>> operator()() {
      return state->rx.recv().then(
        [self = std::move(*this)](Option<bytes::Bytes> chunk) mutable
        -> Promise<Result<bytes::Bytes>> {
          if (chunk.is_none()) {
            return xpp::resolve(
              Result<bytes::Bytes>(xpp::ok, self.state->buf.freeze()));
          }
          auto &&b = chunk.unwrap();
          self.state->buf.put(b.data(), b.size());
          return self(); // tail-recursive
        });
    }
  };

  return DrainLoop{state}();
}

#endif

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_RESPONSE_H
