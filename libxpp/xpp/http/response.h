/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * response.h - xpp::http::Response — unified HTTP response.
 *
 * A single Response type for both server and client sides (like
 * hyper's http::Response, axum's Response).  Use ResponseBuilder
 * to construct, then consume via text()/bytes()/chunk() on the
 * client side, or write_to(ctx) on the server side.
 *
 *   // Builder → Response
 *   Response resp = Response::builder()
 *     .status(200).header("X", "v").body("hello")
 *     .build();
 *
 *   // Shortcut for handlers
 *   return Response::ok("hello").into_promise();
 *
 *   // Client-side consumption
 *   co_await resp.text();
 *   co_await resp.chunk();
 */

#ifndef XPP_HTTP_RESPONSE_H
#define XPP_HTTP_RESPONSE_H

#include <string>
#include <utility>

#include <xpp/bytes/bytes.h>
#include <xpp/bytes/bytes_mut.h>
#include <xpp/compiler.h>
#include <xpp/http/error.h>
#include <xpp/http/header.h>
#include <xpp/io/traits.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/sync/mpsc.h>

#include <x/http/client.h> // xHttpCtx

namespace xpp {
namespace http {

// ═════════════════════════════════════════════════════════════════════
// Forward
// ═════════════════════════════════════════════════════════════════════

// Writer is defined in server.h; Response::stream() captures a
// std::function<void(Writer &)> that is later invoked by write_to().
class Writer;
class Response; // forward-declared for ResponseBuilder

/* ── ResponseBuilder ──────────────────────────────────────────────── */

class ResponseBuilder {
public:
  ResponseBuilder() = default;

  ResponseBuilder &status(int code) {
    m_status = code;
    return *this;
  }

  ResponseBuilder &header(std::string key, std::string value) {
    m_headers.insert(std::move(key), std::move(value));
    return *this;
  }

  /// Streaming body via pull-style TryRead reader.
  ///
  /// The reader's try_read(char*, size_t) → ssize_t method is called
  /// by the server's on_read callback whenever the socket is writable.
  /// Semantics: >0 = data, 0 = EOF, <0 = EAGAIN (try later).
  ///
  /// Accepts any type satisfying the TryRead concept (duck-typing in
  /// C++11, concept-checked in C++20 via xpp::io::TryRead).
  template <XPP_REQUIRES_TRY_READ(R)> ResponseBuilder &body(R &&reader) {
    m_read_fn = Option<std::function<ssize_t(char *, size_t)>>(
      [r = std::forward<R>(reader)](char *buf, size_t cap) mutable -> ssize_t {
        return r.try_read(buf, cap);
      });
    return *this;
  }

  Response build();

  Promise<Result<Response>> into_promise();

private:
  friend class Response;

  int                                            m_status = 200;
  HeaderMap                                      m_headers;
  Option<std::function<ssize_t(char *, size_t)>> m_read_fn;
};

// ═════════════════════════════════════════════════════════════════════
// Response — unified HTTP response (server + client).
//
// Constructed via ResponseBuilder::build().  Body accessed via
// take_try_read() → Option<function<ssize_t(char*,size_t)>>.
// ═════════════════════════════════════════════════════════════════════

class Response {
public:
  static ResponseBuilder builder() { return ResponseBuilder(); }

  Response()                                = default;
  Response(Response &&) noexcept            = default;
  Response &operator=(Response &&) noexcept = default;

  int status() const { return m_status; }

  Option<const std::string &> header(const std::string &name) const { return m_headers.get(name); }

  const HeaderMap &headers() const { return m_headers; }

  bool has_body() const { return m_read_fn.is_some(); }

  Option<std::function<ssize_t(char *, size_t)>> take_try_read() { return std::move(m_read_fn); }

  // ── Convenience body accessors (client side) ─────────────────────

  /// Drain the TryRead body into a UTF-8 string.
  Promise<Result<std::string>> text() {
    auto reader = take_try_read();
    if (reader.is_none()) {
      return xpp::resolve(Result<std::string>(xpp::ok, std::string()));
    }
    auto        fn = std::move(reader).unwrap_unchecked();
    std::string result;
    char        buf[4096];
    while (true) {
      ssize_t n = fn(buf, sizeof(buf));
      if (n <= 0) break;
      result.append(buf, static_cast<size_t>(n));
    }
    return xpp::resolve(Result<std::string>(xpp::ok, std::move(result)));
  }

private:
  friend class ResponseBuilder;

  Response(int status, HeaderMap headers, Option<std::function<ssize_t(char *, size_t)>> read_fn)
      : m_status(std::move(status)), m_headers(std::move(headers)), m_read_fn(std::move(read_fn)) {}

  int                                            m_status = 200;
  HeaderMap                                      m_headers;
  Option<std::function<ssize_t(char *, size_t)>> m_read_fn;
};

// ═════════════════════════════════════════════════════════════════════
// Inline implementations
// ═════════════════════════════════════════════════════════════════════

/* ── ResponseBuilder::build ───────────────────────────────────────── */

inline Response ResponseBuilder::build() {
  if (m_read_fn.is_some()) {
    return Response(m_status, std::move(m_headers), std::move(m_read_fn));
  }
  // No body
  Response r;
  r.m_status  = m_status;
  r.m_headers = std::move(m_headers);
  return r;
}

/* ── ResponseBuilder::into_promise ────────────────────────────────── */

inline Promise<Result<Response>> ResponseBuilder::into_promise() {
  return xpp::resolve(Result<Response>(xpp::ok, build()));
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_RESPONSE_H
