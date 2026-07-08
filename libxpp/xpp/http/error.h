/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h - xpp::http::Error: HTTP-level error type.
 */

#ifndef XPP_HTTP_ERROR_H
#define XPP_HTTP_ERROR_H

#include <string>

#include <xpp/result.h>

namespace xpp {
namespace http {

class Error {
public:
  enum Kind {
    ConnectionFailed,
    Timeout,
    HttpStatus
  };

  static Error connection_failed() {
    return {ConnectionFailed, "connection failed"};
  }
  static Error timeout() {
    return {Timeout, "request timed out"};
  }
  static Error http_status(int code) {
    return {HttpStatus, "HTTP " + std::to_string(code)};
  }

  Kind kind() const {
    return m_kind;
  }
  const std::string &message() const {
    return m_msg;
  }

private:
  Kind        m_kind;
  std::string m_msg;
  Error(Kind k, std::string m) : m_kind(k), m_msg(std::move(m)) {}
};

template <typename T> using Result = xpp::Result<T, Error>;

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_ERROR_H
