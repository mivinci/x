/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * compressed_pair.h - CompressedPair<T1, T2>: stores a pair of values
 *                     with Empty Base Optimization (EBO) when T2 is
 *                     an empty class.
 *
 *   sizeof(CompressedPair<T*, EmptyA>)    == sizeof(T*)         (EBO)
 *   sizeof(CompressedPair<T*, StatefulA>) == sizeof(T*) + sizeof(StatefulA)
 *
 * Used by Box<T, Allocator> / Own<T, Allocator> to store `T* + Allocator` without
 * inflating the handle size when Allocator is empty (e.g. GlobalAllocator).
 * The same strategy is used inside std::unique_ptr by libc++ /
 * libstdc++ / MSVC STL.
 *
 * C++17-compatible. Header-only.
 */

#ifndef XPP_COMPRESSED_PAIR_H
#define XPP_COMPRESSED_PAIR_H

#include <type_traits>
#include <utility>

#include <xpp/meta.h> // for _::is_final

namespace xpp {
namespace _ {

/**
 * @brief Pair of (T1, T2) with EBO when T2 is empty and non-final.
 *
 * Two specializations keyed on `is_empty<T2> && !is_final<T2>`:
 *   - empty + non-final → inherit privately from T2, sizeof == sizeof(T1)
 *   - otherwise         → store T2 as a member, sizeof == sizeof(T1) + sizeof(T2)
 *
 * Both elements are accessed via methods (`first()` / `second()`) so
 * the two specializations have identical public interfaces. Members
 * are private. This mirrors libc++'s `__compressed_pair` design.
 */
template <class T1, class T2, bool UseEbo = std::is_empty<T2>::value && !is_final<T2>::value>
struct CompressedPair {
  CompressedPair() noexcept(std::is_nothrow_default_constructible<T2>::value)
      : m_first(), m_second() {}
  CompressedPair(T1 f, T2 s) noexcept(std::is_nothrow_move_constructible<T2>::value)
      : m_first(std::move(f)), m_second(std::move(s)) {}

  T1 &first() noexcept {
    return m_first;
  }
  const T1 &first() const noexcept {
    return m_first;
  }

  T2 &second() noexcept {
    return m_second;
  }
  const T2 &second() const noexcept {
    return m_second;
  }

private:
  T1 m_first;
  T2 m_second;
};

template <class T1, class T2> struct CompressedPair<T1, T2, true> : private T2 {
  CompressedPair() noexcept(std::is_nothrow_default_constructible<T2>::value) : T2(), m_first() {}
  CompressedPair(T1 f, T2 s) noexcept(std::is_nothrow_move_constructible<T2>::value)
      : T2(std::move(s)), m_first(std::move(f)) {}

  T1 &first() noexcept {
    return m_first;
  }
  const T1 &first() const noexcept {
    return m_first;
  }

  T2 &second() noexcept {
    return *this;
  }
  const T2 &second() const noexcept {
    return *this;
  }

private:
  T1 m_first;
};

} // namespace _
} // namespace xpp

#endif // XPP_COMPRESSED_PAIR_H
