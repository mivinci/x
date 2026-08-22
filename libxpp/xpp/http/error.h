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

#include <string>

#include <xpp/http/status.h>
#include <xpp/option.h>
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
 *
 * When the error originates from a server response with a non-success
 * status code (4xx / 5xx), `status()` returns `Some(code)` and
 * `is_status_error()` returns true. Otherwise `status()` is None.
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

  /** @brief Construct an error with @p kind and a human-readable @p message. */
  Error(Kind kind, String message) : m_kind(kind), m_message(std::move(message)) {}

  /**
   * @brief Construct a status error.
   *
   * Used when the server returned a 4xx/5xx response. @p status is
   * carried alongside the error so callers can recover it via
   * `status()` without parsing the message.
   */
  Error(Kind kind, String message, StatusCode::Value status)
      : m_kind(kind), m_message(std::move(message)), m_status(xpp::some(status)) {}

  Kind kind() const noexcept {
    return m_kind;
  }
  const String &message() const noexcept {
    return m_message;
  }

  /**
   * @brief The HTTP status code associated with the error, if any.
   *
   * Present only for `Kind::Protocol` errors that carry a server
   * response status. None for transport-level errors (Connect/Dns/etc).
   */
  const Option<StatusCode::Value> &status() const noexcept {
    return m_status;
  }

  /**
   * @brief True if the error originated from a server response
   *        (i.e. `status()` is Some).
   *
   * Equivalent to `status().is_some()`. Useful for branching on
   * "transport failed" vs "server returned an error status".
   */
  bool is_status_error() const noexcept {
    return m_status.is_some();
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

  /**
   * @brief Human-readable representation.
   *
   * Format: `"<kind>: <message>"` (or `"<kind>: <message> (status NNN)"` when
   * `status()` is Some). Suitable for logging; not stable for parsing.
   */
  String to_string() const {
    String s = String::from_utf8(kind_name(m_kind)).unwrap();
    s.push_str(String::from_utf8(": ").unwrap());
    s.push_str(m_message);
    if (m_status.is_some()) {
      s.push_str(String::from_utf8(" (status ").unwrap());
      s.push_str(String::from_utf8(std::to_string(static_cast<uint16_t>(m_status.unwrap())).c_str())
                   .unwrap());
      s.push_str(String::from_utf8(")").unwrap());
    }
    return s;
  }

private:
  static const char *kind_name(Kind k) {
    switch (k) {
    case Kind::Connect:
      return "connect";
    case Kind::Dns:
      return "dns";
    case Kind::Timeout:
      return "timeout";
    case Kind::TooManyRedirects:
      return "too-many-redirects";
    case Kind::InvalidUrl:
      return "invalid-url";
    case Kind::Io:
      return "io";
    case Kind::Protocol:
      return "protocol";
    case Kind::Tls:
      return "tls";
    case Kind::Body:
      return "body";
    }
    return "unknown";
  }

  Kind                      m_kind = Kind::Io;
  String                    m_message;
  Option<StatusCode::Value> m_status;
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
