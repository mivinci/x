/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * request.h - xpp::http::Request — immutable client HTTP request.
 *
 * Build via Request::builder().method(Method::Get).url(url).body().unwrap()
 * then pass to Client::send(req).  Like hyper's http::Request.
 *
 *   // Build (no body)
 *   auto req = Request::builder()
 *     .method(Method::Get)
 *     .url(url)
 *     .body()
 *     .unwrap();
 *
 *   // Build (with TryRead body)
 *   auto req = Request::builder()
 *     .method(Method::Post)
 *     .url(url)
 *     .body(bytes::Reader(data))
 *     .unwrap();
 *
 *   // Send
 *   auto resp = co_await client.send(req);
 *
 * body() is the terminal call — it consumes the builder.
 */

#ifndef XPP_HTTP_REQUEST_H
#define XPP_HTTP_REQUEST_H

#include <string>
#include <utility>

#include <xpp/http/error.h>
#include <xpp/http/header.h>
#include <xpp/io/traits.h>
#include <xpp/option.h>
#include <xpp/result.h>

namespace xpp {
namespace http {

// ═════════════════════════════════════════════════════════════════════
// Method
// ═════════════════════════════════════════════════════════════════════

enum class Method {
  Get,
  Post,
  Put,
  Delete,
  Patch,
  Head
};

// ═════════════════════════════════════════════════════════════════════
// Forward
// ═════════════════════════════════════════════════════════════════════

class Request;

// ═════════════════════════════════════════════════════════════════════
// RequestBuilder
// ═════════════════════════════════════════════════════════════════════

class RequestBuilder {
public:
  RequestBuilder &method(Method m) {
    m_method = m;
    return *this;
  }

  RequestBuilder &url(std::string u) {
    m_url = std::move(u);
    return *this;
  }

  RequestBuilder &header(const std::string &key, const std::string &value) {
    m_headers.insert(key, value);
    return *this;
  }

  /// Terminal — no body.
  Result<Request> body();

  /// Terminal — body via TryRead reader.  The reader must satisfy
  /// `try_read(char*, size_t) → ssize_t` (e.g. bytes::Reader).
  template <XPP_REQUIRES_TRY_READ(R)> Result<Request> body(R &&reader);

private:
  friend class Request;

  Method                                  m_method = Method::Get;
  std::string                             m_url;
  HeaderMap m_headers;
};

// ═════════════════════════════════════════════════════════════════════
// Request — immutable client HTTP request.
//
// Body is accessed via take_try_read() (one-shot, consumed by Client::send).
// ═════════════════════════════════════════════════════════════════════

class Request {
public:
  static RequestBuilder builder() {
    return RequestBuilder();
  }

  Method method() const {
    return m_method;
  }
  const std::string &url() const {
    return m_url;
  }
  const HeaderMap &headers() const {
    return m_headers;
  }

  bool has_body() const {
    return m_read_fn.is_some();
  }

  /// Take ownership of the TryRead reader (moved out, one-shot).
  Option<std::function<ssize_t(char *, size_t)>> take_try_read() {
    return std::move(m_read_fn);
  }

private:
  friend class RequestBuilder;

  Method                                         m_method;
  std::string                                    m_url;
  HeaderMap m_headers;
  Option<std::function<ssize_t(char *, size_t)>> m_read_fn;

  Request() = default;

  Request(Method m, std::string url, HeaderMap headers,
          Option<std::function<ssize_t(char *, size_t)>> read_fn)
      : m_method(m), m_url(std::move(url)), m_headers(std::move(headers)),
        m_read_fn(std::move(read_fn)) {}
};

// ═════════════════════════════════════════════════════════════════════
// RequestBuilder::body (inline — defined after Request is complete)
// ═════════════════════════════════════════════════════════════════════

inline Result<Request> RequestBuilder::body() {
  return Result<Request>(xpp::ok, Request(m_method, std::move(m_url), std::move(m_headers), {}));
}

template <XPP_REQUIRES_TRY_READ(R)> Result<Request> RequestBuilder::body(R &&reader) {
  auto fn = Option<std::function<ssize_t(char *, size_t)>>(
    [r = std::forward<R>(reader)](char *buf, size_t cap) mutable -> ssize_t {
      return r.try_read(buf, cap);
    });
  // Treat as "has body" regardless of size — the reader may produce 0 bytes
  return Result<Request>(xpp::ok,
                         Request(m_method, std::move(m_url), std::move(m_headers), std::move(fn)));
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_REQUEST_H
