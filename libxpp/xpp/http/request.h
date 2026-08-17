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

  Method method() const noexcept {
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

  Method    m_method = Method::Get;
  String    m_url;
  HeaderMap m_headers;
  Body      m_body;
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

  RequestBuilder &method(Method m) noexcept {
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
  inline RequestBuilder &basic_auth(String user, String password) {
    // Build "user:pass" first, then base64-encode.
    String credentials = std::move(user);
    credentials.push_str(String::from_utf8(":").unwrap());
    credentials.push_str(password);

    auto bytes = credentials.as_bytes();
    // base64 encodes 3 bytes → 4 chars, ceil(len/3)*4 + 1 for NUL.
    size_t    enc_max = (bytes.size() + 2) / 3 * 4 + 1;
    Vec<char> enc;
    enc.reserve(enc_max);
    size_t enc_len = enc_max;
    int    rc      = xBase64Encode(bytes.data(), bytes.size(), enc.data(), &enc_len);
    XPP_ASSERT(rc == 0, "base64 encode failed (buffer sized correctly)");
    String value        = String::from_utf8(enc.data(), enc_len).unwrap();
    String header_value = String::from_utf8("Basic ").unwrap();
    header_value.push_str(value);
    m_headers.insert(String::from_utf8("Authorization").unwrap(), std::move(header_value));
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

  /* ── Convenience terminators (method + url + empty body) ───────── */

  Result<Request> get(String u) {
    return reset(Method::Get, std::move(u)).body();
  }
  Result<Request> get(const char *u) {
    return reset(Method::Get, u).body();
  }

  Result<Request> post(String u) {
    return reset(Method::Post, std::move(u)).body();
  }
  Result<Request> post(const char *u) {
    return reset(Method::Post, u).body();
  }
  Result<Request> post(String u, Body b) {
    return reset(Method::Post, std::move(u)).body(std::move(b));
  }
  Result<Request> post(const char *u, Body b) {
    return reset(Method::Post, u).body(std::move(b));
  }

  Result<Request> put(String u) {
    return reset(Method::Put, std::move(u)).body();
  }
  Result<Request> put(const char *u) {
    return reset(Method::Put, u).body();
  }
  Result<Request> put(String u, Body b) {
    return reset(Method::Put, std::move(u)).body(std::move(b));
  }
  Result<Request> put(const char *u, Body b) {
    return reset(Method::Put, u).body(std::move(b));
  }

  Result<Request> delete_(String u) {
    return reset(Method::Delete, std::move(u)).body();
  }
  Result<Request> delete_(const char *u) {
    return reset(Method::Delete, u).body();
  }

  Result<Request> patch(String u) {
    return reset(Method::Patch, std::move(u)).body();
  }
  Result<Request> patch(const char *u) {
    return reset(Method::Patch, u).body();
  }
  Result<Request> patch(String u, Body b) {
    return reset(Method::Patch, std::move(u)).body(std::move(b));
  }
  Result<Request> patch(const char *u, Body b) {
    return reset(Method::Patch, u).body(std::move(b));
  }

  Result<Request> head(String u) {
    return reset(Method::Head, std::move(u)).body();
  }
  Result<Request> head(const char *u) {
    return reset(Method::Head, u).body();
  }

private:
  RequestBuilder &reset(Method m, String u) {
    m_method = m;
    m_url    = std::move(u);
    return *this;
  }
  RequestBuilder &reset(Method m, const char *u) {
    m_method = m;
    m_url    = String::from_utf8(u).unwrap();
    return *this;
  }

  Method    m_method = Method::Get;
  String    m_url;
  HeaderMap m_headers;
};

inline RequestBuilder Request::builder() {
  return RequestBuilder();
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_REQUEST_H
