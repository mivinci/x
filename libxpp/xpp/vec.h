/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * vec.h — Vec<T, Alloc>: contiguous growable array.
 *
 * Modeled after Rust's std::vec::Vec, built on xpp's allocator protocol.
 * Replaces std::vector<T> for all xpp-owned code paths.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_VEC_H
#define XPP_VEC_H

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include <xpp/allocator.h>
#include <xpp/compressed_pair.h>
#include <xpp/meta.h>
#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/result.h>
#include <xpp/span.h>

namespace xpp {

template <class T, class Alloc = GlobalAllocator>
class Vec {
  static_assert(!std::is_const<T>::value, "Vec<T>: T must not be const");

  struct RawStorage {
    T*     ptr;
    size_t len;
    size_t cap;
  };

public:
  using value_type     = T;
  using allocator_type = Alloc;

  /* ── Construction ────────────────────────────────────────────────── */

  Vec() noexcept(std::is_nothrow_default_constructible<Alloc>::value) = default;

  explicit Vec(Alloc alloc)
    noexcept(std::is_nothrow_move_constructible<Alloc>::value)
    : m_data(RawStorage{nullptr, 0, 0}, std::move(alloc)) {}

  explicit Vec(size_t capacity, Alloc alloc = Alloc{})
    : m_data(RawStorage{nullptr, 0, 0}, std::move(alloc)) {
    if (capacity > 0) {
      auto r = try_reserve(capacity);
      XPP_ASSERT(r.is_ok(), "Vec(capacity): OOM");
    }
  }

  Vec(const Vec& other)
    : m_data(RawStorage{nullptr, 0, 0}, other.allocator()) {
    if (other.len() == 0) return;
    auto r = try_reserve(other.len());
    XPP_ASSERT(r.is_ok(), "Vec copy: OOM");
    for (size_t i = 0; i < other.len(); ++i) {
      ::new (ptr() + i) T(other[i]);
    }
    len_() = other.len();
  }

  Vec(Vec&& other) noexcept
    : m_data(RawStorage{other.ptr(), other.len(), other.capacity()},
             std::move(other.allocator())) {
    other.ptr_() = nullptr;
    other.len_() = 0;
    other.cap_() = 0;
  }

  ~Vec() {
    destroy_range(0, len());
    dealloc_buffer();
  }

  Vec& operator=(const Vec& other) {
    if (this != &other) {
      clear();
      auto r = try_reserve(other.len());
      XPP_ASSERT(r.is_ok(), "Vec copy=: OOM");
      for (size_t i = 0; i < other.len(); ++i) {
        ::new (ptr() + i) T(other[i]);
      }
      len_() = other.len();
    }
    return *this;
  }

  Vec& operator=(Vec&& other) noexcept {
    if (this != &other) {
      destroy_range(0, len());
      dealloc_buffer();
      ptr_() = other.ptr();
      len_() = other.len();
      cap_() = other.capacity();
      other.ptr_() = nullptr;
      other.len_() = 0;
      other.cap_() = 0;
    }
    return *this;
  }

  /* ── Borrowing ───────────────────────────────────────────────────── */

  Span<T>       as_span()       { return Span<T>(ptr(), len()); }
  Span<const T> as_span() const { return Span<const T>(cptr(), len()); }

  const T* data() const noexcept { return cptr(); }
  T*       data()       noexcept { return ptr(); }

  /* ── Capacity ────────────────────────────────────────────────────── */

  size_t len()      const noexcept { return m_data.first().len; }
  /** Equivalent to len() — provided for generic code compatibility
   *  with templates that expect size(). */
  size_t size()     const noexcept { return len(); }
  size_t capacity() const noexcept { return m_data.first().cap; }
  bool   empty()    const noexcept { return len() == 0; }

  void reserve(size_t additional) {
    auto r = try_reserve(additional);
    XPP_ASSERT(r.is_ok(), "Vec::reserve: OOM");
  }

  Result<void, AllocError> try_reserve(size_t additional) {
    size_t required = len() + additional;
    if (required <= capacity()) return ok;
    return grow_to(required);
  }

  void shrink_to_fit() {
    auto r = try_shrink_to_fit();
    XPP_ASSERT(r.is_ok(), "Vec::shrink_to_fit: OOM");
  }

  Result<void, AllocError> try_shrink_to_fit() {
    if (capacity() <= len()) return ok;

    size_t new_bytes = len() * sizeof(T);
    size_t old_bytes = capacity() * sizeof(T);
    Layout old_layout = Layout::array(old_bytes, alignof(T));
    Layout new_layout = Layout::array(new_bytes, alignof(T));

    if (len() == 0) {
      dealloc_buffer();
      ptr_() = nullptr;
      cap_() = 0;
      return ok;
    }

    auto r = default_shrink(allocator(), static_cast<void*>(ptr()), old_layout, new_layout);
    if (r.is_err()) return Result<void, AllocError>(err, r.unwrap_err());

    Span<uint8_t> new_mem = r.unwrap();
    ptr_() = reinterpret_cast<T*>(new_mem.data());
    cap_() = len();
    return ok;
  }

  /* ── Element Access ──────────────────────────────────────────────── */

  T&       operator[](size_t index)       { XPP_DEBUG_ASSERT(index < len(), "index=%zu len=%zu", index, len()); return ptr()[index]; }
  const T& operator[](size_t index) const { XPP_DEBUG_ASSERT(index < len(), "index=%zu len=%zu", index, len()); return cptr()[index]; }

  Option<T&>       get(size_t index)       { return (index < len()) ? Option<T&>(ptr()[index]) : Option<T&>(none); }
  Option<const T&> get(size_t index) const { return (index < len()) ? Option<const T&>(cptr()[index]) : Option<const T&>(none); }

  Option<T&>       first()       { return (len() > 0) ? Option<T&>(ptr()[0]) : Option<T&>(none); }
  Option<const T&> first() const { return (len() > 0) ? Option<const T&>(cptr()[0]) : Option<const T&>(none); }
  Option<T&>       last()        { return (len() > 0) ? Option<T&>(ptr()[len() - 1]) : Option<T&>(none); }
  Option<const T&> last()  const { return (len() > 0) ? Option<const T&>(cptr()[len() - 1]) : Option<const T&>(none); }

  /* ── Mutation ────────────────────────────────────────────────────── */

  void push(const T& value) {
    auto r = try_push(value);
    XPP_ASSERT(r.is_ok(), "OOM");
  }

  void push(T&& value) {
    auto r = try_push(std::move(value));
    XPP_ASSERT(r.is_ok(), "OOM");
  }

  Result<void, AllocError> try_push(const T& value) {
    if (len() == capacity()) {
      auto r = grow_for_push();
      if (r.is_err()) return r;
    }
    ::new (ptr() + len()) T(value);
    ++len_();
    return ok;
  }

  Result<void, AllocError> try_push(T&& value) {
    if (len() == capacity()) {
      auto r = grow_for_push();
      if (r.is_err()) return r;
    }
    ::new (ptr() + len()) T(std::move(value));
    ++len_();
    return ok;
  }

  void push_unchecked(T&& value) {
    XPP_DEBUG_ASSERT(len() < capacity(), "push_unchecked: not enough capacity (len=%zu cap=%zu)", len(), capacity());
    ::new (ptr() + len()) T(std::move(value));
    ++len_();
  }

  /** Copy into reserved capacity without checking. Caller guarantees
   *  len() < capacity() — otherwise this is UB. */
  void push_unchecked(const T& value) {
    XPP_DEBUG_ASSERT(len() < capacity(), "push_unchecked: not enough capacity (len=%zu cap=%zu)", len(), capacity());
    ::new (ptr() + len()) T(value);
    ++len_();
  }

  Option<T> pop() {
    if (len() == 0) return none;
    --len_();
    T val(std::move(ptr()[len()]));
    ptr()[len()].~T();
    return Option<T>(std::move(val));
  }

  void clear() {
    destroy_range(0, len());
    len_() = 0;
  }

  void truncate(size_t new_len) {
    if (new_len < len()) {
      destroy_range(new_len, len());
      len_() = new_len;
    }
  }

  /* ── Resize ──────────────────────────────────────────────────────── */

  void resize(size_t new_len, const T& fill) {
    auto r = try_resize(new_len, fill);
    XPP_ASSERT(r.is_ok(), "Vec::resize: OOM");
  }

  Result<void, AllocError> try_resize(size_t new_len, const T& fill) {
    if (new_len > len()) {
      auto r = try_reserve(new_len - len());
      if (r.is_err()) return r;
      while (len() < new_len) {
        ::new (ptr() + len()) T(fill);
        ++len_();
      }
    } else if (new_len < len()) {
      truncate(new_len);
    }
    return ok;
  }

  /* ── Append ──────────────────────────────────────────────────────── */

  void append(Vec& other) {
    auto r = try_append(other);
    XPP_ASSERT(r.is_ok(), "Vec::append: OOM");
  }

  Result<void, AllocError> try_append(Vec& other) {
    if (other.len() == 0) return ok;
    auto r = try_reserve(other.len());
    if (r.is_err()) return r;
    for (size_t i = 0; i < other.len(); ++i) {
      ::new (ptr() + len()) T(std::move(other.ptr()[i]));
      ++len_();
    }
    other.clear();
    return ok;
  }

  /** Append copies of elements from another Vec without consuming it. */
  void append(const Vec& other) {
    auto r = try_append(other);
    XPP_ASSERT(r.is_ok(), "Vec::append: OOM");
  }

  Result<void, AllocError> try_append(const Vec& other) {
    if (other.len() == 0) return ok;
    auto r = try_reserve(other.len());
    if (r.is_err()) return r;
    for (size_t i = 0; i < other.len(); ++i) {
      ::new (ptr() + len()) T(other.cptr()[i]);
      ++len_();
    }
    return ok;
  }

  /* ── Extend ──────────────────────────────────────────────────────── */

  /** Append elements from a span. Copies each element via placement-new. */
  void extend_from(Span<const T> items) {
    auto r = try_reserve(static_cast<size_t>(items.size()));
    XPP_ASSERT(r.is_ok(), "Vec::extend_from: OOM");
    for (size_t i = 0; i < static_cast<size_t>(items.size()); ++i) {
      ::new (ptr() + len()) T(items[i]);
      ++len_();
    }
  }

  Result<void, AllocError> try_extend_from(Span<const T> items) {
    auto r = try_reserve(static_cast<size_t>(items.size()));
    if (r.is_err()) return r;
    for (size_t i = 0; i < static_cast<size_t>(items.size()); ++i) {
      ::new (ptr() + len()) T(items[i]);
      ++len_();
    }
    return ok;
  }

  /* ── Split ───────────────────────────────────────────────────────── */

  Vec split_off(size_t at) {
    XPP_ASSERT(at <= len(), "split_off: at=%zu len=%zu", at, len());
    size_t tail_len = len() - at;
    Vec tail(tail_len > 0 ? tail_len : 0, allocator());
    for (size_t i = at; i < len(); ++i) {
      ::new (tail.ptr() + (i - at)) T(std::move(ptr()[i]));
    }
    tail.len_() = tail_len;
    truncate(at);
    return tail;
  }

  /* ── Element Removal ─────────────────────────────────────────────── */

  T swap_remove(size_t index) {
    XPP_ASSERT(index < len(), "swap_remove: index=%zu len=%zu", index, len());
    T val(std::move(ptr()[index]));
    if (index != len() - 1) {
      ::new (ptr() + index) T(std::move(ptr()[len() - 1]));
      ptr()[len() - 1].~T();
    } else {
      ptr()[index].~T();
    }
    --len_();
    return val;
  }

  template <class Pred>
  void retain(Pred pred) {
    size_t write = 0;
    for (size_t read = 0; read < len(); ++read) {
      if (pred(ptr()[read])) {
        if (read != write) {
          ptr()[write] = std::move(ptr()[read]);
        }
        ++write;
      } else {
        ptr()[read].~T();
      }
    }
    len_() = write;
  }

  /* ── Iteration ───────────────────────────────────────────────────── */

  T*       begin()       noexcept { return ptr(); }
  T*       end()         noexcept { return ptr() + len(); }
  const T* begin() const noexcept { return cptr(); }
  const T* end()   const noexcept { return cptr() + len(); }

  /* ── Allocator Access ────────────────────────────────────────────── */

  Alloc&       allocator()       noexcept { return alloc_(); }
  const Alloc& allocator() const noexcept { return alloc_(); }

private:
  _::CompressedPair<RawStorage, Alloc> m_data;

  /* ── Private field accessors ─────────────────────────────────────── */

  T*&       ptr_()        { return m_data.first().ptr; }
  T*        ptr()         { return m_data.first().ptr; }
  const T*  cptr()  const { return m_data.first().ptr; }
  size_t&   len_()        { return m_data.first().len; }
  size_t&   cap_()        { return m_data.first().cap; }
  Alloc&    alloc_()      { return m_data.second(); }
  const Alloc& alloc_() const { return m_data.second(); }

  /* ── Internal helpers ────────────────────────────────────────────── */

  void destroy_range(size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      ptr()[i].~T();
    }
  }

  void dealloc_buffer() {
    if (ptr() && capacity() > 0) {
      allocator().deallocate(static_cast<void*>(ptr()),
                             Layout::array(capacity() * sizeof(T), alignof(T)));
    }
  }

  size_t grow_cap_for(size_t required) const {
    size_t new_cap = capacity() > 0 ? capacity() * 2 : 4;
    while (new_cap < required) new_cap *= 2;
    return new_cap;
  }

  Result<void, AllocError> grow_to(size_t new_cap) {
    if (new_cap <= capacity()) return ok;

    size_t old_bytes = capacity() * sizeof(T);
    size_t new_bytes = new_cap * sizeof(T);

    Layout old_layout = Layout::array(old_bytes, alignof(T));
    Layout new_layout = Layout::array(new_bytes, alignof(T));

    auto r = default_grow(allocator(), static_cast<void*>(ptr()), old_layout, new_layout);
    if (r.is_err()) return Result<void, AllocError>(err, r.unwrap_err());

    Span<uint8_t> new_mem = r.unwrap();
    ptr_() = reinterpret_cast<T*>(new_mem.data());
    cap_() = new_cap;
    return ok;
  }

  Result<void, AllocError> grow_for_push() {
    size_t required = len() + 1;
    if (required > capacity()) {
      return grow_to(grow_cap_for(required));
    }
    return ok;
  }
};

} // namespace xpp

#endif // XPP_VEC_H
