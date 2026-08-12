/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * serde.h - Trait-based serialization framework for libxpp.
 *
 * Modeled on Rust's serde: a data type specializes `Serialize<T>` /
 * `Deserialize<T>` once, and any backend that satisfies the `Serializer`
 * / `Deserializer` concept can drive it. Backends are duck-typed (no
 * vtable) and template-monomorphized per call site.
 *
 * ── Trait primitives ──────────────────────────────────────────────────
 *
 *   template <class T> struct Serialize;     // user specializes
 *   template <class T> struct Deserialize;   // user specializes
 *
 *   template <class T, class S>
 *   Result<Void, Error> serialize(const T&, S&);        // ADL-free dispatcher
 *   template <class T, class D>
 *   Result<T, Error>    deserialize(D&);                 // dispatcher
 *
 * ── Serializer concept (duck-typed, member functions on S) ───────────
 *
 *   Result<Void, Error> serialize_bool(bool);
 *   Result<Void, Error> serialize_i32(int32_t);
 *   Result<Void, Error> serialize_i64(int64_t);
 *   Result<Void, Error> serialize_u32(uint32_t);
 *   Result<Void, Error> serialize_u64(uint64_t);
 *   Result<Void, Error> serialize_f32(float);
 *   Result<Void, Error> serialize_f64(double);
 *   Result<Void, Error> serialize_str(const xpp::String&);
 *   Result<Void, Error> serialize_none();
 *   Result<SeqScope, Error>    serialize_seq(xpp::Option<size_t> len);
 *   Result<StructScope, Error> serialize_struct(const char* name, size_t n);
 *
 * SeqScope / StructScope are RAII handles returned by value. Their API:
 *
 *   StructScope::field(const char* key, const T&)  -> Result<Void, Error>
 *   SeqScope::element(const T&)                    -> Result<Void, Error>
 *   *_Scope::end()                                 -> Result<Void, Error>
 *
 * ── Deserializer concept (duck-typed, member functions on D) ─────────
 *
 *   Result<bool, Error>          deserialize_bool();
 *   Result<int32_t, Error>       deserialize_i32();
 *   Result<int64_t, Error>       deserialize_i64();
 *   Result<uint32_t, Error>      deserialize_u32();
 *   Result<uint64_t, Error>      deserialize_u64();
 *   Result<float, Error>         deserialize_f32();
 *   Result<double, Error>        deserialize_f64();
 *   Result<xpp::String, Error>  deserialize_str();
 *
 *   template <class V>
 *   auto deserialize_option(V&& visitor)
 *     -> decltype(std::declval<V>().visit_none());
 *     // visitor.visit_none()         -> Result<R, Error>
 *     // visitor.visit_some(D&)       -> Result<R, Error>   (same R)
 *
 *   template <class V>
 *   auto deserialize_seq(V&& visitor)
 *     -> decltype(std::declval<V>().visit_seq(std::declval<SeqAccess&>()));
 *     // visitor.visit_seq(SeqAccess&) -> Result<R, Error>
 *
 *   template <class V>
 *   auto deserialize_struct(const char* name,
 *                           const char* const* fields,
 *                           size_t n,
 *                           V&& visitor)
 *     -> decltype(std::declval<V>().visit_map(std::declval<MapAccess&>()));
 *     // visitor.visit_map(MapAccess&) -> Result<R, Error>
 *
 * Return types are deduced via trailing-return-type so visitors do not
 * need a `Value` typedef — they just need the right `visit_*` method.
 *
 * SeqAccess / MapAccess are nested types of the Deserializer (e.g.
 * `json::Deserializer::SeqAccess`) and expose:
 *
 *   template <class T> Result<xpp::Option<T>, Error> SeqAccess::next_element();
 *   Result<xpp::Option<xpp::String>, Error> MapAccess::next_key();
 *   template <class V> Result<V, Error>              MapAccess::next_value();
 *   Result<Void, Error>                              MapAccess::next_value_ignored();
 *
 * C++11-compatible. Header-only. No exceptions. No RTTI.
 */

#ifndef XPP_SERDE_SERDE_H
#define XPP_SERDE_SERDE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/error.h>
#include <xpp/string.h>
#include <xpp/vec.h>
#include <xpp/void.h>

namespace xpp {
namespace serde {

/* ────────────────────────── TRY helpers ──────────────────────────── */

/**
 * @brief Propagate a Result's error: `XPP_SERDE_TRY(expr);` is a
 *        statement that returns the Err from the enclosing function
 *        if `expr` is Err. No-op on Ok.
 *
 * The expression's Ok value is discarded; use XPP_SERDE_TRY_VAR to
 * capture it.
 */
#define XPP_SERDE_TRY(expr)                                                      \
  do {                                                                           \
    auto _xpp_serde_try_tmp = (expr);                                            \
    if (!_xpp_serde_try_tmp.is_ok())                                             \
      return xpp::err(std::move(_xpp_serde_try_tmp).unwrap_err());               \
  } while (0)

/**
 * @brief Declare `var` with the Ok value of `expr`, propagating on Err.
 *
 *   XPP_SERDE_TRY_VAR(name, deserializer.next_value<std::string>());
 *   // `name` is now in scope as std::string
 */
#define XPP_SERDE_TRY_VAR(var, expr)                                             \
  auto _xpp_serde_try_##var = (expr);                                            \
  if (!_xpp_serde_try_##var.is_ok())                                             \
    return xpp::err(std::move(_xpp_serde_try_##var).unwrap_err());               \
  auto var = std::move(_xpp_serde_try_##var).unwrap()

/* ──────────────────────── Primary templates ──────────────────────── */

/**
 * @brief Trait: `T` can serialize itself through any `Serializer`.
 *
 * Specialize for user types. The specialization MUST provide:
 *
 *   template <class S>
 *   static Result<Void, Error> run(const T&, S& serializer);
 */
template <class T>
struct Serialize {
  static_assert(sizeof(T) == 0,
                "Serialize<T> is not specialized for this type. "
                "Either provide a specialization or use the XPP_SERDE macro.");
};

/**
 * @brief Trait: `T` can deserialize itself from any `Deserializer`.
 *
 * Specialize for user types. The specialization MUST provide:
 *
 *   template <class D>
 *   static Result<T, Error> run(D& deserializer);
 */
template <class T>
struct Deserialize {
  static_assert(sizeof(T) == 0,
                "Deserialize<T> is not specialized for this type. "
                "Either provide a specialization or use the XPP_SERDE macro.");
};

/* ──────────────────── Free-function dispatchers ──────────────────── */

/**
 * @brief Serialize `value` through `ser`. Dispatches to
 *        `Serialize<decay<T>>::run(value, ser)`.
 *
 * Call sites use this rather than naming the trait directly, mirroring
 * Rust's `serde::serialize`.
 */
template <class T, class S>
Result<Void, Error> serialize(const T& value, S& ser) {
  return Serialize<typename std::decay<T>::type>::run(value, ser);
}

/**
 * @brief Deserialize a `T` from `deser`. Dispatches to
 *        `Deserialize<T>::run(deser)`.
 */
template <class T, class D>
Result<T, Error> deserialize(D& deser) {
  return Deserialize<T>::run(deser);
}

/* ──────────────── Built-in: primitive specializations ────────────── */

template <>
struct Serialize<bool> {
  template <class S>
  static Result<Void, Error> run(bool v, S& s) {
    return s.serialize_bool(v);
  }
};
template <>
struct Deserialize<bool> {
  template <class D>
  static Result<bool, Error> run(D& d) {
    return d.deserialize_bool();
  }
};

template <>
struct Serialize<int32_t> {
  template <class S>
  static Result<Void, Error> run(int32_t v, S& s) {
    return s.serialize_i32(v);
  }
};
template <>
struct Deserialize<int32_t> {
  template <class D>
  static Result<int32_t, Error> run(D& d) {
    return d.deserialize_i32();
  }
};

template <>
struct Serialize<int64_t> {
  template <class S>
  static Result<Void, Error> run(int64_t v, S& s) {
    return s.serialize_i64(v);
  }
};
template <>
struct Deserialize<int64_t> {
  template <class D>
  static Result<int64_t, Error> run(D& d) {
    return d.deserialize_i64();
  }
};

template <>
struct Serialize<uint32_t> {
  template <class S>
  static Result<Void, Error> run(uint32_t v, S& s) {
    return s.serialize_u32(v);
  }
};
template <>
struct Deserialize<uint32_t> {
  template <class D>
  static Result<uint32_t, Error> run(D& d) {
    return d.deserialize_u32();
  }
};

template <>
struct Serialize<uint64_t> {
  template <class S>
  static Result<Void, Error> run(uint64_t v, S& s) {
    return s.serialize_u64(v);
  }
};
template <>
struct Deserialize<uint64_t> {
  template <class D>
  static Result<uint64_t, Error> run(D& d) {
    return d.deserialize_u64();
  }
};

template <>
struct Serialize<float> {
  template <class S>
  static Result<Void, Error> run(float v, S& s) {
    return s.serialize_f32(v);
  }
};
template <>
struct Deserialize<float> {
  template <class D>
  static Result<float, Error> run(D& d) {
    return d.deserialize_f32();
  }
};

template <>
struct Serialize<double> {
  template <class S>
  static Result<Void, Error> run(double v, S& s) {
    return s.serialize_f64(v);
  }
};
template <>
struct Deserialize<double> {
  template <class D>
  static Result<double, Error> run(D& d) {
    return d.deserialize_f64();
  }
};

template <>
struct Serialize<String> {
  template <class S>
  static Result<Void, Error> run(const String& v, S& s) {
    return s.serialize_str(v);
  }
};
template <>
struct Deserialize<String> {
  template <class D>
  static Result<String, Error> run(D& d) {
    return d.deserialize_str();
  }
};

/* ──────────────── Built-in: Option<T> specialization ─────────────── */

template <class T>
struct Serialize<Option<T>> {
  template <class S>
  static Result<Void, Error> run(const Option<T>& v, S& s) {
    if (v.is_none()) return s.serialize_none();
    // Const-ref into the Some payload; Serialize<T> sees a `const T&`.
    return Serialize<T>::run(v.unwrap(), s);
  }
};

template <class T>
struct Deserialize<Option<T>> {
  template <class D>
  static Result<Option<T>, Error> run(D& d) {
    struct Visitor {
      Result<Option<T>, Error> visit_none() { return ok(Option<T>(none)); }
      Result<Option<T>, Error> visit_some(D& d2) {
        XPP_SERDE_TRY_VAR(v, Deserialize<T>::run(d2));
        return ok(Option<T>(std::move(v)));
      }
    };
    return d.deserialize_option(Visitor{});
  }
};

/* ──────────────── Built-in: Vec<T> specialization ────────────────── */

template <class T>
struct Serialize<Vec<T>> {
  template <class S>
  static Result<Void, Error> run(const Vec<T>& v, S& s) {
    XPP_SERDE_TRY_VAR(seq, s.serialize_seq(Option<size_t>(v.len())));
    for (size_t i = 0; i < v.len(); ++i) {
      XPP_SERDE_TRY(seq.element(v[i]));
    }
    return seq.end();
  }
};

template <class T>
struct Deserialize<Vec<T>> {
  template <class D>
  static Result<Vec<T>, Error> run(D& d) {
    struct Visitor {
      Result<Vec<T>, Error> visit_seq(typename D::SeqAccess& seq) {
        Vec<T> out;
        while (true) {
          XPP_SERDE_TRY_VAR(next, seq.template next_element<T>());
          if (next.is_none()) break;
          out.push(std::move(next.unwrap()));
        }
        return ok(std::move(out));
      }
    };
    return d.deserialize_seq(Visitor{});
  }
};

}  // namespace serde
}  // namespace xpp

#endif  // XPP_SERDE_SERDE_H
