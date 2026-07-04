/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * allocator.h — Allocator protocol for xpp smart pointers.
 *
 * Modeled after Rust's std::alloc::Allocator trait.
 *
 * An Allocator provides:
 *   allocate(Layout) → Result<Span<uint8_t>, AllocError>
 *   deallocate(void*, Layout)
 *   grow(void*, old, new) → Result<Span<uint8_t>, AllocError>     (optional)
 *   shrink(void*, old, new) → Result<Span<uint8_t>, AllocError>   (optional)
 *
 * GlobalAllocator is the default — empty class, EBO-eligible.
 *
 * C++11-compatible. Header-only.
 */
#ifndef XPP_ALLOCATOR_H
#define XPP_ALLOCATOR_H

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

#include <xpp/meta.h>
#include <xpp/result.h>
#include <xpp/span.h>

namespace xpp {

/* ── AllocError ──────────────────────────────────────────────────── */

struct AllocError {};

/* ── Layout ──────────────────────────────────────────────────────── */

struct Layout {
  size_t size;
  size_t align;

  template <class T> static Layout of() {
    return Layout{sizeof(T), alignof(T)};
  }

  static Layout array(size_t n, size_t a) {
    return Layout{n, a};
  }

  bool operator==(const Layout &o) const {
    return size == o.size && align == o.align;
  }
};

/* ── AllocInfo removed — use Span<uint8_t> directly ────────────── */
/* allocate() returns Result<Span<uint8_t>, AllocError> — the Span  */
/* carries both pointer and actual allocated size (>= layout.size).   */

/* ── GlobalAllocator ─────────────────────────────────────────────── */

struct GlobalAllocator {
  Result<Span<uint8_t>, AllocError> allocate(Layout layout) const {
#if __cplusplus >= 201703L || (defined(_MSC_VER) && _MSVC_LANG >= 201703L)
    void *p = ::operator new(layout.size, std::align_val_t(layout.align), std::nothrow);
#else
    (void)layout.align;
    void *p = ::operator new(layout.size, std::nothrow);
#endif
    if (!p) return Result<Span<uint8_t>, AllocError>(err, AllocError{});
    return Result<Span<uint8_t>, AllocError>(ok,
                                             Span<uint8_t>(static_cast<uint8_t *>(p), layout.size));
  }

  void deallocate(void *ptr, Layout layout) const {
    (void)layout;
#if __cplusplus >= 201703L || (defined(_MSC_VER) && _MSVC_LANG >= 201703L)
    ::operator delete(ptr, layout.size, std::align_val_t(layout.align));
#else
    ::operator delete(ptr);
#endif
  }
};

/* ── Default grow / shrink ───────────────────────────────────────── */
/*
 * Allocators may optionally provide grow() and shrink() methods.
 * If not provided, use these free functions as defaults:
 * allocate new, memcpy, deallocate old.
 *
 * To use a custom grow/shrink, call alloc.grow(...) directly.
 */

template <class Allocator>
Result<Span<uint8_t>, AllocError> default_grow(const Allocator &alloc, void *ptr, Layout old_l,
                                               Layout new_l) {
  auto r = alloc.allocate(new_l);
  if (r.is_err()) return r;
  std::memcpy(r.unwrap().data(), ptr, old_l.size);
  alloc.deallocate(ptr, old_l);
  return r;
}

template <class Allocator>
Result<Span<uint8_t>, AllocError> default_shrink(const Allocator &alloc, void *ptr, Layout old_l,
                                                 Layout new_l) {
  auto r = alloc.allocate(new_l);
  if (r.is_err()) return r;
  std::memcpy(r.unwrap().data(), ptr, new_l.size);
  alloc.deallocate(ptr, old_l);
  return r;
}

namespace _ {

/* ── FirstIsAlloc — SFINAE helper for make() ──────────────────────── */
// True iff the first arg in Args... is convertible to Allocator.
// Used to disambiguate make(alloc, args...) from make(args...).
template <class Allocator, class... Args>
struct FirstIsAlloc : std::false_type {}; // empty pack → false

template <class Allocator, class First, class... Rest>
struct FirstIsAlloc<Allocator, First, Rest...>
    : std::integral_constant<
        bool, std::is_convertible<typename std::decay<First>::type, Allocator>::value> {};

/* ── destroy_and_dealloc — helper for Box/Own dtors ───────────────── */
// Calls ~T() (if T is not void) then alloc.deallocate(ptr, Layout::of<T>()).
// For T = void, skips the destructor and uses Layout{0, 1} as a sentinel
// (GlobalAllocator::deallocate ignores the layout for the actual free).
template <class T, class Allocator>
inline void destroy_and_dealloc(T *ptr, Allocator &alloc, std::false_type /*is_void*/) noexcept {
  ptr->~T();
  alloc.deallocate(ptr, Layout::of<T>());
}

template <class T, class Allocator>
inline void destroy_and_dealloc(T *ptr, Allocator &alloc, std::true_type /*is_void*/) noexcept {
  // void has no destructor; use sentinel layout.
  alloc.deallocate(ptr, Layout{0, 1});
}

template <class T, class Allocator>
inline void destroy_and_dealloc(T *ptr, Allocator &alloc) noexcept {
  destroy_and_dealloc<T, Allocator>(ptr, alloc, std::is_void<T>{});
}

} // namespace _

} // namespace xpp

#endif // XPP_ALLOCATOR_H
