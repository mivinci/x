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
    Builder, // Invalid config, bad URL, etc.
    Request, // Connection failed, network error
    Timeout, // Request timed out
    Status,  // HTTP error status code
  };

  static Error builder(const std::string &msg) {
    return {Builder, msg};
  }
  static Error request(const std::string &msg) {
    return {Request, msg};
  }
  static Error timeout(const std::string &msg) {
    return {Timeout, msg};
  }
  static Error status_code(int code) {
    return {Status, "HTTP " + std::to_string(code)};
  }

  Kind kind() const {
    return m_kind;
  }
  const std::string &message() const {
    return m_msg;
  }

  bool is_builder() const {
    return m_kind == Builder;
  }
  bool is_request() const {
    return m_kind == Request;
  }
  bool is_timeout() const {
    return m_kind == Timeout;
  }
  bool is_status() const {
    return m_kind == Status;
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
