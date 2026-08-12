/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h - Error type for the serde framework.
 *
 * All fallible serde operations return xpp::Result<T, xpp::serde::Error>.
 * Errors are never thrown.
 *
 * Naming follows Rust serde's `de::Error::custom` / `ser::Error::custom`:
 * the single factory name is `custom`, with overloads for "untyped"
 * (kind = Custom, matches Rust's only required trait method) and
 * "typed" (explicit ErrorKind, an xpp extension that lets callers
 * pattern-match on the failure category — something Rust achieves via
 * `Error::source()` + downcasting on concrete backend error types).
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_SERDE_ERROR_H
#define XPP_SERDE_ERROR_H

#include <cstddef>
#include <utility>

#include <xpp/result.h>
#include <xpp/string.h>

namespace xpp {
namespace serde {

/**
 * @brief Broad category of a serde failure.
 *
 * xpp extension: Rust serde does not expose an error-kind enum on the
 * trait — callers downcast `Error::source()` to a concrete backend
 * type. xpp prefers an open discriminant so that backends and users
 * can pattern-match without RTTI. `Error::custom()` defaults to
 * `ErrorKind::Custom`; the `custom(kind, msg)` overload lets a backend
 * tag the error more precisely.
 */
enum class ErrorKind {
  Unexpected,   ///< Unexpected token or structural mismatch.
  Eof,          ///< Input ended before a value was fully read.
  InvalidValue, ///< Value present but wrong type or out of range.
  MissingField, ///< Required struct field absent on deserialize.
  UnknownField, ///< Input contains a field not declared on the type.
  Custom,       ///< Backend- or user-defined error via `Error::custom`.
};

/**
 * @brief Serde error value. Carried in Result<T, Error> by every trait
 *        method and every Serializer/Deserializer method.
 *
 * Mirrors Rust serde's `Error::custom<T: Display>(msg)` — the single
 * factory entry point called `custom`. Two overload sets:
 *
 *   // "untyped" — matches Rust's required trait method
 *   Error::custom("missing 'age'");
 *   Error::custom(some_string);
 *
 *   // "typed" — xpp extension for callers that pattern-match on kind
 *   Error::custom(ErrorKind::MissingField, "missing 'age'");
 *   Error::custom(ErrorKind::Eof, some_string);
 *
 * `message` is xpp::String (byte-accurate, supports embedded NULs, and
 * keeps the serde headers free of std::string's <string> dependency).
 * String-literal overloads use String::from_utf8 under the hood —
 * non-UTF-8 input yields an empty String rather than an exception.
 */
struct Error {
  ErrorKind kind;
  String    message;

  /* ── "untyped" overloads — kind = Custom ────────────────────────── */

  /** @brief Custom error from a String (move-friendly). */
  static Error custom(String msg) {
    return custom(ErrorKind::Custom, std::move(msg));
  }

  /** @brief Custom error from a NUL-terminated UTF-8 literal. */
  static Error custom(const char *msg) {
    return custom(ErrorKind::Custom, msg);
  }

  /* ── "typed" overloads — explicit ErrorKind ─────────────────────── */

  /** @brief Typed error from a String (move-friendly). */
  static Error custom(ErrorKind k, String msg) {
    Error e;
    e.kind    = k;
    e.message = std::move(msg);
    return e;
  }

  /** @brief Typed error from a NUL-terminated UTF-8 literal. */
  static Error custom(ErrorKind k, const char *msg) {
    return custom(k, _make_string(msg));
  }

private:
  /** @brief Best-effort String construction; non-UTF-8 yields empty String. */
  static String _make_string(const char *s) {
    if (!s) return String();
    auto r = String::from_utf8(s);
    return r.is_ok() ? std::move(r).unwrap() : String();
  }
};

/**
 * @brief Free-function shorthand for constructing an Error.
 *
 * Mirrors the `ok`/`err` tag pattern used elsewhere in libxpp so that
 * returning an error reads as:
 *
 *   return xpp::err(serde::error(ErrorKind::Eof, "unexpected end"));
 *
 * Equivalent to `Error::custom(kind, msg)`. Provided for call sites
 * that prefer the free-function form; `Error::custom` is the canonical
 * entry point matching Rust serde's trait method name.
 */
inline Error error(ErrorKind k, String msg) {
  return Error::custom(k, std::move(msg));
}

/** @brief String-literal overload — common case in error paths. */
inline Error error(ErrorKind k, const char *msg) {
  return Error::custom(k, msg);
}

} // namespace serde
} // namespace xpp

#endif // XPP_SERDE_ERROR_H
