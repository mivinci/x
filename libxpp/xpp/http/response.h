/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * response.h - HTTP response value type + fluent builder.
 */

#ifndef XPP_HTTP_RESPONSE_H
#define XPP_HTTP_RESPONSE_H

#include <utility>

#include <xpp/http/body.h>
#include <xpp/http/header.h>
#include <xpp/http/status.h>
#include <xpp/option.h>
#include <xpp/string.h>

namespace xpp {
namespace http {

class ResponseBuilder;

/**
 * @brief HTTP response value.
 *
 * Owns status, headers, body, and (optionally) the final URL after redirects.
 *
 * Usually constructed via `Response::builder()`:
 *
 * @code
 *   Response r = Response::builder()
 *                   .status(StatusCode::Ok)
 *                   .header("Content-Type", "text/plain")
 *                   .body("hello")
 *                   .build();
 * @endcode
 *
 * Or via static convenience constructors:
 * @code
 *   Response r = Response::ok("hello");
 * @endcode
 */
class Response {
public:
  Response()                                = default;
  Response(Response &&) noexcept            = default;
  Response &operator=(Response &&) noexcept = default;
  Response(const Response &)                = delete;
  Response &operator=(const Response &)     = delete;

  static ResponseBuilder builder();

  /* ── Status line ──────────────────────────────────────────────── */

  StatusCode status() const noexcept {
    return m_status;
  }
  uint16_t status_code() const noexcept {
    return static_cast<uint16_t>(m_status);
  }

  /* ── Headers ──────────────────────────────────────────────────── */

  const HeaderMap &headers() const noexcept {
    return m_headers;
  }

  /// Case-insensitive header lookup. Returns None if absent.
  Option<const String &> header(const String &name) const {
    return m_headers.get(name);
  }
  Option<const String &> header(const char *name) const {
    return m_headers.get(name);
  }

  /* ── Body ────────────────────────────────────────────────────── */

  Body &body() noexcept {
    return m_body;
  }
  Body into_body() noexcept {
    return std::move(m_body);
  }
  bool has_body() const noexcept {
    return !m_body.is_empty();
  }

  /**
   * @brief Consume the body into a single `Bytes`.
   *
   * Convenience: equivalent to `into_body().bytes()`. The Response is
   * left with an empty body after this call.
   */
  Promise<http::Result<Bytes>> bytes();

  /**
   * @brief Consume the body and decode as UTF-8.
   *
   * Returns Err(http::Error{Kind::Body, ...}) if the body is not valid UTF-8.
   */
  Promise<http::Result<String>> text();

  /* ── Redirect tracking ────────────────────────────────────────── */

  /// Final URL after following redirects. None if no redirects occurred.
  const Option<String> &url() const noexcept {
    return m_final_url;
  }

private:
  friend class ResponseBuilder;

  StatusCode     m_status = StatusCode::Ok;
  HeaderMap      m_headers;
  Body           m_body;
  Option<String> m_final_url;
};

/**
 * @brief Fluent builder for `Response`.
 *
 * Unlike `RequestBuilder` (which returns `Result<Request>` from terminators
 * because future body sources may fail to construct), `ResponseBuilder`
 * returns `Response` directly — `Body::from` overloads used here cannot fail.
 */
class ResponseBuilder {
public:
  ResponseBuilder()                                       = default;
  ResponseBuilder(ResponseBuilder &&) noexcept            = default;
  ResponseBuilder &operator=(ResponseBuilder &&) noexcept = default;
  ResponseBuilder(const ResponseBuilder &)                = delete;
  ResponseBuilder &operator=(const ResponseBuilder &)     = delete;

  /* ── Configurators ────────────────────────────────────────────── */

  ResponseBuilder &status(StatusCode code) noexcept {
    m_status = code;
    return *this;
  }

  ResponseBuilder &status(uint16_t code) noexcept {
    m_status = static_cast<StatusCode>(code);
    return *this;
  }

  ResponseBuilder &header(String key, String value) {
    m_headers.insert(std::move(key), std::move(value));
    return *this;
  }

  ResponseBuilder &header(const char *key, const char *value) {
    XPP_ASSERT(key != nullptr && value != nullptr, "ResponseBuilder::header(nullptr)");
    m_headers.insert(String::from_utf8(key).unwrap(), String::from_utf8(value).unwrap());
    return *this;
  }

  /* ── Terminators ──────────────────────────────────────────────── */

  Response body(Body b) {
    Response r;
    r.m_status  = m_status;
    r.m_headers = std::move(m_headers);
    r.m_body    = std::move(b);
    return r;
  }

  Response body(Bytes bytes) {
    return body(Body::from(std::move(bytes)));
  }
  Response body(Vec<uint8_t> bytes) {
    return body(Body::from(std::move(bytes)));
  }
  Response body(String text) {
    return body(Body::from(std::move(text)));
  }
  Response body(const char *text) {
    XPP_ASSERT(text != nullptr, "ResponseBuilder::body(nullptr)");
    return body(Body::from(text));
  }

  Response body() {
    return body(Body::empty());
  }

  /* ── Static convenience constructors ──────────────────────────── */

  /// 200 OK with body.
  static Response ok(Bytes body) {
    return ResponseBuilder().status(StatusCode::Ok).body(std::move(body));
  }
  static Response ok(String body) {
    return ResponseBuilder().status(StatusCode::Ok).body(std::move(body));
  }
  static Response ok(const char *body) {
    return ResponseBuilder().status(StatusCode::Ok).body(body);
  }
  /// 200 OK, empty body.
  static Response ok() {
    return ResponseBuilder().status(StatusCode::Ok).body();
  }

  /// 201 Created with body.
  static Response created(Bytes body) {
    return ResponseBuilder().status(StatusCode::Created).body(std::move(body));
  }
  static Response created(String body) {
    return ResponseBuilder().status(StatusCode::Created).body(std::move(body));
  }
  static Response created(const char *body) {
    return ResponseBuilder().status(StatusCode::Created).body(body);
  }

  /// 204 No Content, empty body.
  static Response no_content() {
    return ResponseBuilder().status(StatusCode::NoContent).body();
  }

  /// 400 Bad Request with body.
  static Response bad_request(Bytes body) {
    return ResponseBuilder().status(StatusCode::BadRequest).body(std::move(body));
  }
  static Response bad_request(String body) {
    return ResponseBuilder().status(StatusCode::BadRequest).body(std::move(body));
  }
  static Response bad_request(const char *body) {
    return ResponseBuilder().status(StatusCode::BadRequest).body(body);
  }

  /// 404 Not Found, empty body.
  static Response not_found() {
    return ResponseBuilder().status(StatusCode::NotFound).body();
  }

  /// 500 Internal Server Error with body.
  static Response internal_server_error(Bytes body) {
    return ResponseBuilder().status(StatusCode::InternalServerError).body(std::move(body));
  }
  static Response internal_server_error(String body) {
    return ResponseBuilder().status(StatusCode::InternalServerError).body(std::move(body));
  }
  static Response internal_server_error(const char *body) {
    return ResponseBuilder().status(StatusCode::InternalServerError).body(body);
  }

private:
  StatusCode m_status = StatusCode::Ok;
  HeaderMap  m_headers;
};

inline ResponseBuilder Response::builder() {
  return ResponseBuilder();
}

/* ── Response::bytes() / text() — move body out and aggregate ──── */

inline Promise<http::Result<Bytes>> Response::bytes() {
  return into_body().bytes();
}

inline Promise<http::Result<String>> Response::text() {
  return into_body().text();
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_RESPONSE_H
