/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * http.h - Umbrella header for the xpp::http module + top-level
 *          convenience functions (xpp::http::get/post/...).
 *
 * Mirrors reqwest's top-level functions (reqwest::get etc.). Each call
 * constructs a default Client and delegates to the corresponding
 * Client method. The default Client requires an active EventLoop
 * (WaitScope) — calls without one return Err.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_H
#define XPP_HTTP_H

#include <xpp/http/client.h>
#include <xpp/http/error.h>
#include <xpp/http/header.h>
#include <xpp/http/method.h>
#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/http/status.h>

namespace xpp {
namespace http {

namespace _ {

/// The default Client, reused per thread (design.md Q1).
///
/// A temporary Client would be destroyed right after the request is
/// submitted, and xHttpClientDestroy cancels in-flight requests — so the
/// top-level functions must keep their Client alive across the whole
/// transfer. A thread-local Client also avoids per-call curl setup cost.
///
/// @warning The Client is bound to the EventLoop current at first use.
/// Keep that loop alive for the lifetime of the thread's top-level calls.
inline Client &default_client() {
  static thread_local Client c = Client::builder().build().unwrap_or(Client());
  return c;
}

/// Release the thread-local default Client (used by tests that tear down
/// their EventLoop before the thread ends; the Client's destructor would
/// otherwise touch a destroyed loop).
inline void reset_default_client() {
  default_client() = Client();
}

} // namespace _

/** @brief GET @p url (no body). See design.md Q1 for the per-call cost. */
template <class Url> inline Promise<http::Result<Response>> get(Url &&url) {
  return _::default_client().get(std::forward<Url>(url));
}

/** @brief POST @p url, empty body. */
template <class Url> inline Promise<http::Result<Response>> post(Url &&url) {
  return _::default_client().post(std::forward<Url>(url));
}
/** @brief POST @p url with @p body (Bytes/String/Vec<uint8_t>/const char* / Body). */
template <class Url, class B> inline Promise<http::Result<Response>> post(Url &&url, B &&body) {
  return _::default_client().post(std::forward<Url>(url), std::forward<B>(body));
}

/** @brief PUT @p url, empty body. */
template <class Url> inline Promise<http::Result<Response>> put(Url &&url) {
  return _::default_client().put(std::forward<Url>(url));
}
/** @brief PUT @p url with @p body. */
template <class Url, class B> inline Promise<http::Result<Response>> put(Url &&url, B &&body) {
  return _::default_client().put(std::forward<Url>(url), std::forward<B>(body));
}

/**
 * @brief DELETE @p url (no body).
 *
 * Named `del` — `delete` is a C++ keyword.
 */
template <class Url> inline Promise<http::Result<Response>> del(Url &&url) {
  return _::default_client().del(std::forward<Url>(url));
}

/** @brief PATCH @p url, empty body. */
template <class Url> inline Promise<http::Result<Response>> patch(Url &&url) {
  return _::default_client().patch(std::forward<Url>(url));
}
/** @brief PATCH @p url with @p body. */
template <class Url, class B> inline Promise<http::Result<Response>> patch(Url &&url, B &&body) {
  return _::default_client().patch(std::forward<Url>(url), std::forward<B>(body));
}

/** @brief HEAD @p url (no body). */
template <class Url> inline Promise<http::Result<Response>> head(Url &&url) {
  return _::default_client().head(std::forward<Url>(url));
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_H
