/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * method.h — HTTP request methods (RFC 9110 §9.1).
 *
 * Mirrors hyper::Method / reqwest::Method. The eight HTTP methods are
 * modeled as an enum class with to_string / from_string conversions.
 * from_string accepts case-insensitive ASCII input ("GET", "get", "Get"
 * all map to Method::Get) and returns None on unknown tokens.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_METHOD_H
#define XPP_HTTP_METHOD_H

#include <cstdint>

#include <xpp/option.h>
#include <xpp/string.h>

namespace xpp {
namespace http {

/**
 * @brief HTTP request method.
 *
 * Enum class with a fixed uint8_t underlying type so it can be stored
 * compactly in Request.
 */
enum class Method : uint8_t {
  Get,
  Post,
  Put,
  Delete,
  Patch,
  Head,
  Options,
  Trace,
  Connect,
};

/**
 * @brief Canonical uppercase method name (e.g. Method::Get → "GET").
 *
 * Returns a borrowed C string literal — no allocation.
 */
inline const char *to_string(Method m) noexcept {
  switch (m) {
  case Method::Get:
    return "GET";
  case Method::Post:
    return "POST";
  case Method::Put:
    return "PUT";
  case Method::Delete:
    return "DELETE";
  case Method::Patch:
    return "PATCH";
  case Method::Head:
    return "HEAD";
  case Method::Options:
    return "OPTIONS";
  case Method::Trace:
    return "TRACE";
  case Method::Connect:
    return "CONNECT";
  }
  return ""; // unreachable
}

namespace _ {

/// ASCII case-insensitive comparison against a lowercase reference token.
/// Accepts "GET", "get", "Get". Only ASCII letters are folded.
inline bool eq_ci_ascii(const char *p, size_t n, const char *lower_token) {
  // lower_token is a lowercase literal like "get"; strlen gives its length.
  size_t i = 0;
  for (; i < n; ++i) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != lower_token[i]) return false;
  }
  // Must have consumed exactly the token — no prefix matches.
  return lower_token[i] == '\0';
}

} // namespace _

/**
 * @brief Parse a method token (case-insensitive ASCII).
 *
 * "GET" / "get" / "Get" → Method::Get. Unknown tokens return None.
 * Non-ASCII bytes always return None.
 */
inline Option<Method> from_string(const String &s) noexcept {
  const char *p = reinterpret_cast<const char *>(s.as_bytes().data());
  size_t      n = s.len();

  // RFC 9110 method tokens are case-sensitive, but servers and proxies
  // commonly receive mixed case; accepting it here matches reqwest/hyper.
  if (_::eq_ci_ascii(p, n, "get")) return Method::Get;
  if (_::eq_ci_ascii(p, n, "post")) return Method::Post;
  if (_::eq_ci_ascii(p, n, "put")) return Method::Put;
  if (_::eq_ci_ascii(p, n, "delete")) return Method::Delete;
  if (_::eq_ci_ascii(p, n, "patch")) return Method::Patch;
  if (_::eq_ci_ascii(p, n, "head")) return Method::Head;
  if (_::eq_ci_ascii(p, n, "options")) return Method::Options;
  if (_::eq_ci_ascii(p, n, "trace")) return Method::Trace;
  if (_::eq_ci_ascii(p, n, "connect")) return Method::Connect;
  return none;
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_METHOD_H
