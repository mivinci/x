/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * response.h - xpp::http::Response: HTTP response with async body.
 *
 * Resolved at header time. Body access:
 *   - text() / bytes()  — wait for the full body (buffered).
 *   - chunk()           — stream bytes as they arrive (mpcs channel).
 *
 * Mutually exclusive: once you start reading chunks, the buffered
 * path is unavailable, and vice versa.
 */

#ifndef XPP_HTTP_RESPONSE_H
#define XPP_HTTP_RESPONSE_H

#include <map>
#include <string>
#include <utility>

#include <xpp/bytes/bytes.h>
#include <xpp/http/error.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/result.h>
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
    return std::move(m_full_body);
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

  /// Set the buffered body promise (resolves on on_done).
  void set_full_body(Promise<Result<bytes::Bytes>> p) {
    m_full_body = std::move(p);
  }

  /// Set the streaming body channel (populated by on_data, closed by on_done).
  void set_body_channel(sync::mpsc::Receiver<bytes::Bytes> rx) {
    m_body_rx = Option<sync::mpsc::Receiver<bytes::Bytes>>(std::move(rx));
  }

private:
  enum class Access { None, Buffered, Streaming };

  int                                     m_status  = 0;
  Access                                  m_access  = Access::None;
  std::multimap<std::string, std::string> m_headers;
  Promise<Result<bytes::Bytes>>           m_full_body;
  Option<sync::mpsc::Receiver<bytes::Bytes>> m_body_rx;
};

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_RESPONSE_H
