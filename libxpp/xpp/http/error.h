/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h — xpp::http::Error + http::Result<T> alias.
 *
 * Error is the canonical error type for the http module. Result<T> is
 * an alias for Result<T, Error> so that Body::bytes() / Client::send() /
 * etc. can return Promise<Result<T>> without spelling out the error type.
 *
 * Mirrors reqwest::Error / hyper::Error. C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_ERROR_H
#define XPP_HTTP_ERROR_H

#include <xpp/result.h>
#include <xpp/string.h>

namespace xpp {
namespace http {

/**
 * @brief HTTP error.
 *
 * Covers connect / DNS / timeout / redirect / URL / I/O / protocol /
 * TLS / body errors. The kind field classifies the error for
 * predicate-style checks (is_timeout, is_connect, etc.).
 */
class Error {
public:
  enum class Kind {
    Connect,
    Dns,
    Timeout,
    TooManyRedirects,
    InvalidUrl,
    Io,
    Protocol,
    Tls,
    Body,
  };

  Error() = default;
  Error(Kind kind, String message) : m_kind(kind), m_message(std::move(message)) {}

  Kind kind() const noexcept {
    return m_kind;
  }
  const String &message() const noexcept {
    return m_message;
  }

  bool is_connect() const noexcept {
    return m_kind == Kind::Connect;
  }
  bool is_dns() const noexcept {
    return m_kind == Kind::Dns;
  }
  bool is_timeout() const noexcept {
    return m_kind == Kind::Timeout;
  }
  bool is_redirect() const noexcept {
    return m_kind == Kind::TooManyRedirects;
  }
  bool is_invalid_url() const noexcept {
    return m_kind == Kind::InvalidUrl;
  }
  bool is_io() const noexcept {
    return m_kind == Kind::Io;
  }
  bool is_protocol() const noexcept {
    return m_kind == Kind::Protocol;
  }
  bool is_tls() const noexcept {
    return m_kind == Kind::Tls;
  }
  bool is_body() const noexcept {
    return m_kind == Kind::Body;
  }

private:
  Kind   m_kind = Kind::Io;
  String m_message;
};

/**
 * @brief Result alias for the http module — default error type is http::Error.
 *
 * Usage:
 *   Result<Response>   r = client.send(req).await();   // Result<Response, Error>
 *   Result<Bytes>      b = body.bytes().await();
 */
template <class T> using Result = xpp::Result<T, Error>;

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_ERROR_H
