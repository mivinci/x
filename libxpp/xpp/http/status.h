/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * status.h — HTTP status codes (RFC 9110 §15).
 *
 * Mirrors hyper::StatusCode. Stores the numeric code as uint16_t and
 * exposes the standard classification predicates (is_informational /
 * is_success / is_redirect / is_client_error / is_server_error) plus
 * to_string (canonical reason phrase) and from_string (parse a token
 * like "404" or "404 Not Found" — accepts leading digits only).
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_STATUS_H
#define XPP_HTTP_STATUS_H

#include <cstdint>

#include <xpp/option.h>
#include <xpp/string.h>

namespace xpp::http {

/**
 * @brief HTTP response status code.
 *
 * Underlying type is uint16_t so it fits in the response object without
 * padding. Codes outside the well-known list can still be constructed
 * via `static_cast<StatusCode>(code)` — predicates classify by range.
 */
enum class StatusCode : uint16_t {
  // 1xx — Informational
  Continue           = 100,
  SwitchingProtocols = 101,
  Processing         = 102,
  EarlyHints         = 103,

  // 2xx — Success
  Ok                          = 200,
  Created                     = 201,
  Accepted                    = 202,
  NonAuthoritativeInformation = 203,
  NoContent                   = 204,
  ResetContent                = 205,
  PartialContent              = 206,
  MultiStatus                 = 207,
  AlreadyReported             = 208,
  ImUsed                      = 226,

  // 3xx — Redirection
  MultipleChoices   = 300,
  MovedPermanently  = 301,
  Found             = 302,
  SeeOther          = 303,
  NotModified       = 304,
  UseProxy          = 305,
  TemporaryRedirect = 307,
  PermanentRedirect = 308,

  // 4xx — Client Error
  BadRequest                  = 400,
  Unauthorized                = 401,
  PaymentRequired             = 402,
  Forbidden                   = 403,
  NotFound                    = 404,
  MethodNotAllowed            = 405,
  NotAcceptable               = 406,
  ProxyAuthenticationRequired = 407,
  RequestTimeout              = 408,
  Conflict                    = 409,
  Gone                        = 410,
  LengthRequired              = 411,
  PreconditionFailed          = 412,
  PayloadTooLarge             = 413,
  UriTooLong                  = 414,
  UnsupportedMediaType        = 415,
  RangeNotSatisfiable         = 416,
  ExpectationFailed           = 417,
  MisdirectedRequest          = 421,
  UnprocessableEntity         = 422,
  Locked                      = 423,
  FailedDependency            = 424,
  TooEarly                    = 425,
  UpgradeRequired             = 426,
  PreconditionRequired        = 428,
  TooManyRequests             = 429,
  RequestHeaderFieldsTooLarge = 431,
  UnavailableForLegalReasons = 451,

  // 5xx — Server Error
  InternalServerError           = 500,
  NotImplemented                = 501,
  BadGateway                    = 502,
  ServiceUnavailable            = 503,
  GatewayTimeout                = 504,
  HttpVersionNotSupported       = 505,
  VariantAlsoNegotiates         = 506,
  InsufficientStorage           = 507,
  LoopDetected                  = 508,
  NotExtended                   = 510,
  NetworkAuthenticationRequired = 511,
};

/* ── Classification predicates ────────────────────────────────────── */

/// @brief 1xx — informational (rarely seen in practice).
inline bool is_informational(StatusCode s) noexcept {
  return static_cast<uint16_t>(s) / 100 == 1;
}

/// @brief 2xx — success.
inline bool is_success(StatusCode s) noexcept {
  return static_cast<uint16_t>(s) / 100 == 2;
}

/// @brief 3xx — redirection.
inline bool is_redirect(StatusCode s) noexcept {
  return static_cast<uint16_t>(s) / 100 == 3;
}

/// @brief 4xx — client error.
inline bool is_client_error(StatusCode s) noexcept {
  return static_cast<uint16_t>(s) / 100 == 4;
}

/// @brief 5xx — server error.
inline bool is_server_error(StatusCode s) noexcept {
  return static_cast<uint16_t>(s) / 100 == 5;
}

/**
 * @brief Canonical reason phrase (RFC 9110 §15).
 *
 * Returns a borrowed C string literal. Unknown codes return "".
 */
inline const char* to_string(StatusCode s) noexcept {
  switch (s) {
    // 1xx
    case StatusCode::Continue:           return "Continue";
    case StatusCode::SwitchingProtocols: return "Switching Protocols";
    case StatusCode::Processing:         return "Processing";
    case StatusCode::EarlyHints:         return "Early Hints";
    // 2xx
    case StatusCode::Ok:                          return "OK";
    case StatusCode::Created:                     return "Created";
    case StatusCode::Accepted:                    return "Accepted";
    case StatusCode::NonAuthoritativeInformation: return "Non-Authoritative Information";
    case StatusCode::NoContent:                   return "No Content";
    case StatusCode::ResetContent:                return "Reset Content";
    case StatusCode::PartialContent:              return "Partial Content";
    case StatusCode::MultiStatus:                 return "Multi-Status";
    case StatusCode::AlreadyReported:             return "Already Reported";
    case StatusCode::ImUsed:                      return "IM Used";
    // 3xx
    case StatusCode::MultipleChoices:   return "Multiple Choices";
    case StatusCode::MovedPermanently:  return "Moved Permanently";
    case StatusCode::Found:             return "Found";
    case StatusCode::SeeOther:          return "See Other";
    case StatusCode::NotModified:       return "Not Modified";
    case StatusCode::UseProxy:          return "Use Proxy";
    case StatusCode::TemporaryRedirect: return "Temporary Redirect";
    case StatusCode::PermanentRedirect: return "Permanent Redirect";
    // 4xx
    case StatusCode::BadRequest:                  return "Bad Request";
    case StatusCode::Unauthorized:                 return "Unauthorized";
    case StatusCode::PaymentRequired:              return "Payment Required";
    case StatusCode::Forbidden:                   return "Forbidden";
    case StatusCode::NotFound:                    return "Not Found";
    case StatusCode::MethodNotAllowed:            return "Method Not Allowed";
    case StatusCode::NotAcceptable:               return "Not Acceptable";
    case StatusCode::ProxyAuthenticationRequired: return "Proxy Authentication Required";
    case StatusCode::RequestTimeout:              return "Request Timeout";
    case StatusCode::Conflict:                    return "Conflict";
    case StatusCode::Gone:                        return "Gone";
    case StatusCode::LengthRequired:              return "Length Required";
    case StatusCode::PreconditionFailed:           return "Precondition Failed";
    case StatusCode::PayloadTooLarge:             return "Payload Too Large";
    case StatusCode::UriTooLong:                  return "URI Too Long";
    case StatusCode::UnsupportedMediaType:        return "Unsupported Media Type";
    case StatusCode::RangeNotSatisfiable:         return "Range Not Satisfiable";
    case StatusCode::ExpectationFailed:           return "Expectation Failed";
    case StatusCode::MisdirectedRequest:          return "Misdirected Request";
    case StatusCode::UnprocessableEntity:          return "Unprocessable Entity";
    case StatusCode::Locked:                       return "Locked";
    case StatusCode::FailedDependency:             return "Failed Dependency";
    case StatusCode::TooEarly:                     return "Too Early";
    case StatusCode::UpgradeRequired:              return "Upgrade Required";
    case StatusCode::PreconditionRequired:         return "Precondition Required";
    case StatusCode::TooManyRequests:              return "Too Many Requests";
    case StatusCode::RequestHeaderFieldsTooLarge:  return "Request Header Fields Too Large";
    case StatusCode::UnavailableForLegalReasons:   return "Unavailable For Legal Reasons";
    // 5xx
    case StatusCode::InternalServerError:           return "Internal Server Error";
    case StatusCode::NotImplemented:                return "Not Implemented";
    case StatusCode::BadGateway:                    return "Bad Gateway";
    case StatusCode::ServiceUnavailable:           return "Service Unavailable";
    case StatusCode::GatewayTimeout:                return "Gateway Timeout";
    case StatusCode::HttpVersionNotSupported:       return "HTTP Version Not Supported";
    case StatusCode::VariantAlsoNegotiates:         return "Variant Also Negotiates";
    case StatusCode::InsufficientStorage:           return "Insufficient Storage";
    case StatusCode::LoopDetected:                  return "Loop Detected";
    case StatusCode::NotExtended:                   return "Not Extended";
    case StatusCode::NetworkAuthenticationRequired: return "Network Authentication Required";
  }
  return "";
}

namespace _ {

/// Parse leading ASCII digits from @p s up to 3 digits (100–599 range).
/// Returns the integer, or 0 if no digits found.
inline uint16_t parse_leading_digits(const char* p, size_t n) noexcept {
  uint32_t v = 0;
  size_t   i = 0;
  while (i < n && i < 3 && p[i] >= '0' && p[i] <= '9') {
    v = v * 10 + static_cast<uint32_t>(p[i] - '0');
    ++i;
  }
  return (i == 0) ? 0 : static_cast<uint16_t>(v);
}

}  // namespace _

/**
 * @brief Parse a status code from a string.
 *
 * Accepts "404", "404 Not Found", "  200  ". Leading digits are parsed,
 * trailing text is ignored. Returns None if no digits found or the code
 * is outside 100–599.
 */
inline Option<StatusCode> from_string(const String& s) noexcept {
  const char* p = reinterpret_cast<const char*>(s.as_bytes().data());
  size_t      n = s.len();

  // Skip leading whitespace.
  size_t i = 0;
  while (i < n && (p[i] == ' ' || p[i] == '\t')) ++i;
  if (i == n) return none;

  uint16_t code = _::parse_leading_digits(p + i, n - i);
  if (code < 100 || code > 599) return none;
  return static_cast<StatusCode>(code);
}

}  // namespace xpp::http

#endif  // XPP_HTTP_STATUS_H
