/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * response.h - xpp::http::Response: HTTP response with async body.
 *
 * Resolved at header time — text() / bytes() are async and wait
 * for the body to complete.
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

namespace xpp {
namespace http {

class Response {
public:
  Response() = default;

  int status() const {
    return m_status;
  }

  Option<std::string> header(const std::string &name) const {
    auto it = m_headers.find(name);
    if (it != m_headers.end()) return Option<std::string>(it->second);
    return none;
  }

  /// Async: wait for full body, then decode as UTF-8.
  Promise<Result<std::string>> text() {
    return std::move(m_body).then([](Result<bytes::Bytes> r) -> Result<std::string> {
      if (r.is_err()) return xpp::err(r.unwrap_err());
      return xpp::ok(r.unwrap_unchecked().to_string());
    });
  }

  /// Async: wait for full body, return raw bytes.
  Promise<Result<bytes::Bytes>> bytes() {
    return std::move(m_body);
  }

  // Internal: set by HttpAdapter in send() before resolve.
  void set_body(Promise<Result<bytes::Bytes>> p) {
    m_body = std::move(p);
  }
  void set_status(int s) {
    m_status = s;
  }

private:
  int                                     m_status = 0;
  std::multimap<std::string, std::string> m_headers;
  Promise<Result<bytes::Bytes>>           m_body;
};

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_RESPONSE_H
