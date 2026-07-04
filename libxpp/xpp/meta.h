/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * meta.h - Metaprogramming helpers that shim C++14/17 traits on
 *          C++11 toolchains. Naming follows the STL convention
 *          (snake_case, matching std::is_final etc.).
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_META_H
#define XPP_META_H

#include <type_traits>

namespace xpp {
namespace _ {

/* ── is_final — portable std::is_final ────────────────────────────── */
// std::is_final is C++14. On C++11 toolchains we fall back to the
// __is_final compiler intrinsic (clang, gcc 4.7+, MSVC) which the
// stdlib's own is_final wraps; on truly ancient toolchains we
// degrade to "assume not final", which at worst forces the
// member-storage specialization for an EBO-eligible type (a
// size-not-correctness issue).
#if __cplusplus >= 201402L
template <class T> struct is_final : std::is_final<T> {};
#elif defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
template <class T> struct is_final {
  static constexpr bool value = __is_final(T);
};
#else
template <class T> struct is_final {
  static constexpr bool value = false;
};
#endif

} // namespace _
} // namespace xpp

#endif // XPP_META_H
