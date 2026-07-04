/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * own.h - Own<T, Allocator>: a nullable owning smart pointer with
 *         Rust-style / std::unique_ptr-style API.
 *
 * Storage is Option<Box<T, Allocator>> directly, so:
 *   sizeof(Own<T>)          == sizeof(T*)              (GlobalAllocator is empty → EBO)
 *   sizeof(Own<T, StatefulA>) == sizeof(T*) + sizeof(StatefulA)
 *
 * Relationship to Box<T>:
 *   - Own<T>          — may be null. Default ctor → null. reset()/release()/take().
 *                       operator* and operator-> debug-assert on null.
 *   - Box<T>          — type-level non-null. No reset, no default ctor, no null state.
 *   - Bridge:
 *       std::move(own).into_nonnull()           -> Option<Box<T, A>>
 *       Own<T>(std::move(opt_box))             <- adopt back from Option
 *
 * Choose Own<T> when you want C++/Rust-idiomatic ownership (operator*, reset,
 * if(o)). Choose Box<T> + Option when you want type-level guarantees
 * and Rust-style combinators.
 *
 * C++17-compatible. Header-only.
 */

#ifndef XPP_OWN_H
#define XPP_OWN_H

#include <type_traits>
#include <utility>

#include <xpp/allocator.h>
#include <xpp/box.h>
#include <xpp/option.h>
#include <xpp/panic.h>

namespace xpp {

/**
 * @brief Nullable owning smart pointer. Rust/std::unique_ptr-style API.
 *
 * Move-only. Exception-free destructor. Holds at most one heap-allocated T,
 * disposed via Allocator on destruction or reset.
 *
 * @tparam T     Pointee type. T = void supported (operator*, ->
 *               SFINAE-removed).
 * @tparam Allocator Allocator used for deallocation. Defaults to
 *               GlobalAllocator (empty, EBO → sizeof(Own<T>) == sizeof(T*)).
 */
template <class T, class Allocator = GlobalAllocator> class Own {
  using Inner = Option<Box<T, Allocator>>;

public:
  using element_type   = T;
  using allocator_type = Allocator;
  using pointer        = T *;

  /** @brief Default ctor. Constructs an empty (null) Own. */
  Own() noexcept = default;

  /** @brief Construct empty from nullptr_t literal. */
  Own(std::nullptr_t) noexcept {}

  /**
   * @brief Take ownership of a raw pointer (may be null).
   *
   * If `p` is null, the resulting Own is empty. Otherwise it owns `p`.
   */
  explicit Own(T *p) noexcept : m_inner(Box<T, Allocator>::try_from_raw(p)) {}

  /** @brief Take ownership of a raw pointer with a custom allocator instance. */
  Own(T *p, Allocator a) noexcept : m_inner(Box<T, Allocator>::try_from_raw(p, std::move(a))) {}

  /** @brief Adopt an existing Box (always non-empty). */
  Own(Box<T, Allocator> &&nn) noexcept : m_inner(std::move(nn)) {}

  /** @brief Adopt from Option<Box>. Empty iff the Option is None. */
  Own(Option<Box<T, Allocator>> &&opt) noexcept : m_inner(std::move(opt)) {}

  /** @brief Covariant: adopt Box<U, Allocator>. */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Own(Box<U, Allocator> &&nn) noexcept : m_inner(std::move(nn)) {}

  /** @brief Covariant: adopt Option<Box<U, Allocator>>. */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Own(Option<Box<U, Allocator>> &&opt) noexcept : m_inner(std::move(opt)) {}

  /** @brief Covariant: Own<U, Allocator> → Own<T, Allocator>. */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Own(Own<U, Allocator> &&other) noexcept : m_inner(std::move(other.m_inner)) {}

  Own(const Own &)            = delete;
  Own &operator=(const Own &) = delete;

  Own(Own &&) noexcept            = default;
  Own &operator=(Own &&) noexcept = default;

  ~Own() = default;

  /** @brief Reset to empty; deletes any currently held object. */
  Own &operator=(std::nullptr_t) noexcept {
    m_inner = none;
    return *this;
  }

  /** @brief Replace held pointer. Old object (if any) is deleted. */
  void reset(T *p = nullptr) noexcept {
    m_inner = Box<T, Allocator>::try_from_raw(p);
  }

  /**
   * @brief Relinquish ownership; return raw pointer (may be null).
   *
   * Rust-style name. Equivalent to `release()`.
   */
  T *take() noexcept {
    if (m_inner.is_none()) return nullptr;
    return std::move(m_inner).unwrap_unchecked().into_raw();
  }

  /**
   * @brief Relinquish ownership; return raw pointer (may be null).
   *
   * std::unique_ptr-style name. Equivalent to `take()`.
   */
  T *release() noexcept {
    return take();
  }

  /** @brief Get raw pointer; null if empty. */
  T *get() const noexcept {
    return m_inner.is_some() ? m_inner.unwrap_unchecked() : nullptr;
  }

  /** @brief Dereference. Debug-asserts non-empty; UB in release on empty. */
  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_inner.is_some(), "Own::operator* on empty Own");
    return *m_inner.unwrap_unchecked();
  }

  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_inner.is_some(), "Own::operator-> on empty Own");
    return m_inner.unwrap_unchecked();
  }

  /** @brief True iff non-empty. */
  explicit operator bool() const noexcept {
    return m_inner.is_some();
  }

  bool operator==(std::nullptr_t) const noexcept {
    return m_inner.is_none();
  }
  bool operator!=(std::nullptr_t) const noexcept {
    return m_inner.is_some();
  }

  /**
   * @brief Consume into Option<Box>. Bridges to the Rust-style API.
   *
   * If the Own was empty, returns None. Otherwise Some(Box). To
   * inspect or take the allocator, do
   * `std::move(own).into_nonnull().unwrap().allocator()`.
   */
  Option<Box<T, Allocator>> into_nonnull() && noexcept {
    return std::move(m_inner);
  }

private:
  Inner m_inner;

  // Allow covariant ctor to reach into another instantiation's storage.
  template <class, class> friend class Own;
};

/* ── Compile-time size guarantees ────────────────────────────────────── */

static_assert(sizeof(Own<int>) == sizeof(int *),
              "Own<T, GlobalAllocator> must be sizeof(T*) via niche-optimized Option storage");

} // namespace xpp

#endif // XPP_OWN_H
