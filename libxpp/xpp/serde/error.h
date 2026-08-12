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
 * C++11-compatible. Header-only.
 */

#ifndef XPP_SERDE_ERROR_H
#define XPP_SERDE_ERROR_H

#include <string>

#include <xpp/result.h>

namespace xpp {
namespace serde {

/**
 * @brief Broad category of a serde failure.
 *
 * Mirrors the discriminant of Rust serde's `serde::de::Error` /
 * `serde::ser::Error` collapsed into one set. Callers MAY pattern-match
 * on this to drive behavior; the `message` field carries human context.
 */
enum class ErrorKind {
  Unexpected,     ///< Unexpected token or structural mismatch.
  Eof,            ///< Input ended before a value was fully read.
  InvalidValue,   ///< Value present but wrong type or out of range.
  MissingField,   ///< Required struct field absent on deserialize.
  UnknownField,   ///< Input contains a field not declared on the type.
  Custom,         ///< Backend- or user-defined error via `Error::custom`.
};

/**
 * @brief Serde error value. Carried in Result<T, Error> by every trait
 *        method and every Serializer/Deserializer method.
 *
 * Construct with the free function `Error::make(kind, message)` or the
 * `serde::error(kind, msg)` shorthand. Both deduce the Error type so
 * call sites read naturally:
 *
 *   return xpp::err(serde::error(ErrorKind::MissingField, "missing 'age'"));
 */
struct Error {
  ErrorKind kind;
  std::string message;

  static Error make(ErrorKind k, std::string msg) {
    Error e;
    e.kind = k;
    e.message = std::move(msg);
    return e;
  }

  /**
   * @brief Build an Error with ErrorKind::Custom and the given message.
   *
   * Convenience for backends and user code that does not fit a more
   * specific kind.
   */
  static Error custom(std::string msg) {
    return make(ErrorKind::Custom, std::move(msg));
  }
};

/**
 * @brief Free-function shorthand for constructing an Error.
 *
 * Mirrors the `ok`/`err` tag pattern used elsewhere in libxpp so that
 * returning an error reads as:
 *
 *   return xpp::err(serde::error(ErrorKind::Eof, "unexpected end"));
 */
inline Error error(ErrorKind k, std::string msg) {
  return Error::make(k, std::move(msg));
}

}  // namespace serde
}  // namespace xpp

#endif  // XPP_SERDE_ERROR_H
