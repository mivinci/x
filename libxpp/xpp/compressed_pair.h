/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * compressed_pair.h - CompressedPair<T1, T2>: stores a pair of values
 *                     with Empty Base Optimization (EBO) when T2 is
 *                     an empty class.
 *
 *   sizeof(CompressedPair<T*, EmptyA>)  == sizeof(T*)         (EBO)
 *   sizeof(CompressedPair<T*, StatefulA>) == sizeof(T*) + sizeof(StatefulA)
 *
 * Used by Box<T, Alloc> / Own<T, Alloc> to store `T* + Alloc` without
 * inflating the handle size when Alloc is empty (e.g. GlobalAllocator).
 * The same strategy is used inside std::unique_ptr by libc++ /
 * libstdc++ / MSVC STL.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_COMPRESSED_PAIR_H
#define XPP_COMPRESSED_PAIR_H

#include <type_traits>
#include <utility>

#include <xpp/allocator.h> // for _::IsFinal

namespace xpp {
namespace _ {

/**
 * @brief Pair of (T, A) with EBO when A is empty and non-final.
 *
 * Two specializations keyed on `is_empty<A> && !is_final<A>`:
 *   - empty + non-final → inherit privately from A, sizeof == sizeof(T)
 *   - otherwise         → store A as a member, sizeof == sizeof(T) + sizeof(A)
 *
 * The first template parameter T is stored as a member in both
 * specializations; only A is EBO-eligible (as a base class). This
 * matches the layout of std::unique_ptr's storage: the pointer is
 * the primary member, the deleter/allocator is the EBO candidate.
 *
 * Accessor is named `allocator()` because the only current use is
 * storing (T*, Alloc) pairs in Box/Own. If CompressedPair is later
 * reused for non-allocator purposes, rename to `second()` or add a
 * generic accessor.
 */
template <class T, class A, bool UseEbo = std::is_empty<A>::value && !IsFinal<A>::value>
struct CompressedPair {
  T p;
  A a;

  CompressedPair() noexcept(std::is_nothrow_default_constructible<A>::value) : p(), a() {}
  CompressedPair(T p_, A a_) noexcept(std::is_nothrow_move_constructible<A>::value)
      : p(std::move(p_)), a(std::move(a_)) {}

  A &allocator() noexcept {
    return a;
  }
  const A &allocator() const noexcept {
    return a;
  }
};

template <class T, class A> struct CompressedPair<T, A, true> : private A {
  T p;

  CompressedPair() noexcept(std::is_nothrow_default_constructible<A>::value) : A(), p() {}
  CompressedPair(T p_, A a_) noexcept(std::is_nothrow_move_constructible<A>::value)
      : A(std::move(a_)), p(std::move(p_)) {}

  A &allocator() noexcept {
    return *this;
  }
  const A &allocator() const noexcept {
    return *this;
  }
};

} // namespace _
} // namespace xpp

#endif // XPP_COMPRESSED_PAIR_H
