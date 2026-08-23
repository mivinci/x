/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * request.h - HTTP request value type + fluent builder.
 */

#ifndef XPP_HTTP_REQUEST_H
#define XPP_HTTP_REQUEST_H

#include <utility>

#include <xpp/http/body.h>
#include <xpp/http/header.h>
#include <xpp/http/method.h>
#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/string.h>

#include <x/base/base64.h>

namespace xpp {
namespace http {

class RequestBuilder;

/**
 * @brief HTTP request value.
 *
 * A `Request` owns its method, URL, headers, and body. Bodies follow the
 * move-only `Body` semantics — use `body()` to borrow for streaming reads,
 * `into_body()` to take ownership for consumption.
 *
 * Usually constructed via `Request::builder()`:
 *
 * @code
 *   auto req = Request::builder()
 *                  .method(Method::Post)
 *                  .url("https://api.example.com")
 *                  .header("Content-Type", "application/json")
 *                  .body(R"({"k":"v"})")
 *                  .unwrap();
 * @endcode
 */
class Request {
public:
  Request()                               = default;
  Request(Request &&) noexcept            = default;
  Request &operator=(Request &&) noexcept = default;
  Request(const Request &)                = delete;
  Request &operator=(const Request &)     = delete;

  static RequestBuilder builder();

  /* ── Accessors ─────────────────────────────────────────────────── */

  Method::Value method() const noexcept {
    return m_method;
  }
  const String &url() const noexcept {
    return m_url;
  }
  const HeaderMap &headers() const noexcept {
    return m_headers;
  }

  Body &body() noexcept {
    return m_body;
  }
  Body into_body() noexcept {
    return std::move(m_body);
  }
  bool has_body() const noexcept {
    return !m_body.is_empty();
  }

private:
  friend class RequestBuilder;

  Method::Value m_method = Method::Get;
  String        m_url;
  HeaderMap     m_headers;
  Body          m_body;
};

/**
 * @brief Fluent builder for `Request`.
 *
 * Chain `method()` / `url()` / `header()` / auth helpers, then terminate
 * with a `body(...)` overload (or a convenience terminator like `get(url)`)
 * to produce a `Result<Request>`.
 *
 * The builder is move-only (carries `HeaderMap` + `Body` which are move-only).
 */
class RequestBuilder {
public:
  RequestBuilder()                                      = default;
  RequestBuilder(RequestBuilder &&) noexcept            = default;
  RequestBuilder &operator=(RequestBuilder &&) noexcept = default;
  RequestBuilder(const RequestBuilder &)                = delete;
  RequestBuilder &operator=(const RequestBuilder &)     = delete;

  /* ── Configurators ────────────────────────────────────────────── */

  RequestBuilder &method(Method::Value m) noexcept {
    m_method = m;
    return *this;
  }

  RequestBuilder &url(String u) {
    m_url = std::move(u);
    return *this;
  }

  RequestBuilder &url(const char *u) {
    XPP_ASSERT(u != nullptr, "RequestBuilder::url(nullptr)");
    m_url = String::from_utf8(u).unwrap();
    return *this;
  }

#if __cpp_lib_string_view
  RequestBuilder &url(std::string_view u) {
    m_url = String::from_utf8(u.data(), u.size()).unwrap();
    return *this;
  }
#endif

  RequestBuilder &header(String key, String value) {
    m_headers.insert(std::move(key), std::move(value));
    return *this;
  }

  RequestBuilder &header(const char *key, const char *value) {
    XPP_ASSERT(key != nullptr && value != nullptr, "RequestBuilder::header(nullptr)");
    m_headers.insert(String::from_utf8(key).unwrap(), String::from_utf8(value).unwrap());
    return *this;
  }

  /**
   * @brief Set `Authorization: Bearer <token>`.
   *
   * Convenience for OAuth/JWT-style auth. No encoding is applied — the
   * token is inserted verbatim.
   */
  RequestBuilder &bearer_auth(String token) {
    String value = String::from_utf8("Bearer ").unwrap();
    value.push_str(token);
    m_headers.insert(String::from_utf8("Authorization").unwrap(), std::move(value));
    return *this;
  }

  /**
   * @brief Set `Authorization: Basic <base64(user:pass)>`.
   *
   * Encodes the credentials with RFC 7617 base64. The caller does not
   * need to pre-encode.
   */
  RequestBuilder &basic_auth(String user, String password) {
    m_headers.insert(String::from_utf8("Authorization").unwrap(),
                     _::basic_auth_value(std::move(user), std::move(password)));
    return *this;
  }

  /* ── Body terminators (return Result<Request>) ─────────────────── */

  Result<Request> body(Body b) {
    Request r;
    r.m_method  = m_method;
    r.m_url     = std::move(m_url);
    r.m_headers = std::move(m_headers);
    r.m_body    = std::move(b);
    return Result<Request>(xpp::ok, std::move(r));
  }

  Result<Request> body(Bytes bytes) {
    return body(Body::from(std::move(bytes)));
  }
  Result<Request> body(Vec<uint8_t> bytes) {
    return body(Body::from(std::move(bytes)));
  }
  Result<Request> body(String text) {
    return body(Body::from(std::move(text)));
  }
  Result<Request> body(const char *text) {
    XPP_ASSERT(text != nullptr, "RequestBuilder::body(nullptr)");
    return body(Body::from(text));
  }

  /// Empty body terminator — useful for GET / HEAD / DELETE.
  Result<Request> body() {
    return body(Body::empty());
  }

private:
  Method::Value m_method = Method::Get;
  String        m_url;
  HeaderMap     m_headers;
};

inline RequestBuilder Request::builder() {
  return RequestBuilder();
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_REQUEST_H
