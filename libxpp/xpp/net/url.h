/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * url.h - xpp::net::Url: RAII wrapper around xUrl.
 *
 * Url::parse() returns Result<Url, xErrno>. The destructor calls
 * xUrlFree. Accessors return std::string / uint16_t.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NET_URL_H
#define XPP_NET_URL_H

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <xpp/result.h>

#include <x/base/error.h>
#include <x/net/url.h>

namespace xpp {
namespace net {

/* ── UrlParseError ────────────────────────────────────────────────── */

/**
 * @brief Errors returned by Url::parse.
 *
 * Mirrors the granularity of Rust's url::ParseError (a subset — libx's
 * parser is a single-shot C function that reports xErrno_InvalidArg, so
 * we distinguish only what we can infer from the input).
 */
enum class UrlParseError : uint8_t {
  Empty,         ///< Input was NULL or empty.
  InvalidFormat, ///< Not a valid URL (missing scheme, host, or malformed).
};

/** @brief Return a human-readable description of a UrlParseError. */
inline const char *url_error_message(UrlParseError e) noexcept {
  switch (e) {
  case UrlParseError::Empty:
    return "empty URL string";
  case UrlParseError::InvalidFormat:
    return "invalid URL format";
  default:
    return "unknown URL parse error";
  }
}

/* ── Url ──────────────────────────────────────────────────────────── */

/**
 * @brief RAII wrapper around xUrl.
 *
 * Parses a URL string into its components (scheme, host, port, path,
 * query, fragment). The internal xUrl owns a copy of the input string;
 * ~Url() releases it via xUrlFree.
 *
 * Move-only.
 */
class Url {
public:
  /** @brief Parse a URL. Returns Err on invalid input. */
  static Result<Url, UrlParseError> parse(const char *raw) {
    if (raw == nullptr || *raw == '\0') {
      return Result<Url, UrlParseError>(xpp::err, UrlParseError::Empty);
    }
    Url    u;
    xErrno err = xUrlParse(raw, &u.m_url);
    if (err != xErrno_Ok) {
      return Result<Url, UrlParseError>(xpp::err, UrlParseError::InvalidFormat);
    }
    return Result<Url, UrlParseError>(xpp::ok, std::move(u));
  }

  /** @brief Parse a URL from a std::string. */
  static Result<Url, UrlParseError> parse(const std::string &raw) {
    if (raw.empty()) {
      return Result<Url, UrlParseError>(xpp::err, UrlParseError::Empty);
    }
    return parse(raw.c_str());
  }

  Url() = default;

  ~Url() {
    if (m_url.raw_) xUrlFree(&m_url);
  }

  Url(Url &&o) noexcept : m_url(o.m_url) {
    o.m_url = xUrl{};
  }
  Url &operator=(Url &&o) noexcept {
    if (this != &o) {
      if (m_url.raw_) xUrlFree(&m_url);
      m_url   = o.m_url;
      o.m_url = xUrl{};
    }
    return *this;
  }
  Url(const Url &)            = delete;
  Url &operator=(const Url &) = delete;

  /* ── Accessors ─────────────────────────────────────────────────── */

  /** @brief URL scheme (e.g. "https"). */
  std::string scheme() const {
    return str(m_url.scheme, m_url.scheme_len);
  }
  /** @brief User info (e.g. "user:pass"). */
  std::string userinfo() const {
    return str(m_url.userinfo, m_url.userinfo_len);
  }
  /** @brief Hostname. */
  std::string host() const {
    return str(m_url.host, m_url.host_len);
  }
  /** @brief Port as a string (e.g. "443"). */
  std::string port() const {
    return str(m_url.port, m_url.port_len);
  }
  /** @brief URL path (e.g. "/api/v1"). */
  std::string path() const {
    return str(m_url.path, m_url.path_len);
  }
  /** @brief Query string without '?' (e.g. "key=val"). */
  std::string query() const {
    return str(m_url.query, m_url.query_len);
  }
  /** @brief Fragment without '#' (e.g. "section1"). */
  std::string fragment() const {
    return str(m_url.fragment, m_url.fragment_len);
  }

  /** @brief Numeric port — explicit if present, else scheme default. */
  uint16_t port_num() const {
    return xUrlPort(&m_url);
  }

  /** @brief Raw xUrl (for advanced use). */
  const xUrl &raw() const {
    return m_url;
  }

private:
  xUrl m_url{};

  static std::string str(const char *p, size_t len) {
    if (!p || len == 0) return std::string();
    return std::string(p, len);
  }
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_URL_H
