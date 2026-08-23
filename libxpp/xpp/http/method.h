/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * method.h — HTTP request methods (RFC 9110 §9.1).
 *
 * Mirrors hyper::Method / reqwest::Method. `Method` is a namespace:
 * `Method::Value` is the underlying enum type and the nine methods are
 * re-exported as constexpr values (`Method::Get`, `Method::Post`, ...)
 * so callers write `Method::Get` exactly as with an enum class.
 * `Method::from_string` accepts case-insensitive ASCII input ("GET",
 * "get", "Get" all map to Method::Get) and returns None on unknown
 * tokens. `to_string(Method::Value)` is a free function in xpp::http.
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
 * @brief HTTP request methods (RFC 9110 §9.1).
 *
 * A namespace rather than an enum class so the enum type and its
 * parsing function can share the `Method::` prefix:
 *
 *     Method::Value m = Method::Get;
 *     auto parsed = Method::from_string("get");   // Option<Method::Value>
 */
namespace Method {

/**
 * @brief Underlying enum for the HTTP method.
 *
 * Enum class with a fixed uint8_t underlying type so it can be stored
 * compactly in Request.
 */
enum class Value : uint8_t {
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

/** @brief Canonical enumerator values, re-exported so callers can write
 *         `Method::Get` instead of `Method::Value::Get`. */
constexpr Value Get     = Value::Get;
constexpr Value Post    = Value::Post;
constexpr Value Put     = Value::Put;
constexpr Value Delete  = Value::Delete;
constexpr Value Patch   = Value::Patch;
constexpr Value Head    = Value::Head;
constexpr Value Options = Value::Options;
constexpr Value Trace   = Value::Trace;
constexpr Value Connect = Value::Connect;

/**
 * @brief Parse a method token (case-insensitive ASCII).
 *
 * "GET" / "get" / "Get" → Method::Get. Unknown tokens return None.
 * Non-ASCII bytes always return None.
 */
inline Option<Value> from_string(const String &s) noexcept {
  const char *p = reinterpret_cast<const char *>(s.as_bytes().data());
  size_t      n = s.len();

  // RFC 9110 method tokens are case-sensitive, but servers and proxies
  // commonly receive mixed case; accepting it here matches reqwest/hyper.
  if (_::eq_ci_ascii(p, n, "get")) return Value::Get;
  if (_::eq_ci_ascii(p, n, "post")) return Value::Post;
  if (_::eq_ci_ascii(p, n, "put")) return Value::Put;
  if (_::eq_ci_ascii(p, n, "delete")) return Value::Delete;
  if (_::eq_ci_ascii(p, n, "patch")) return Value::Patch;
  if (_::eq_ci_ascii(p, n, "head")) return Value::Head;
  if (_::eq_ci_ascii(p, n, "options")) return Value::Options;
  if (_::eq_ci_ascii(p, n, "trace")) return Value::Trace;
  if (_::eq_ci_ascii(p, n, "connect")) return Value::Connect;
  return none;
}

} // namespace Method

/**
 * @brief Canonical uppercase method name (e.g. Method::Get → "GET").
 *
 * Returns a borrowed C string literal — no allocation.
 */
inline const char *to_string(Method::Value m) noexcept {
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

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_METHOD_H
