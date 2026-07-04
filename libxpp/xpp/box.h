/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * box.h - Box<T, Allocator>: a guaranteed-non-null owning smart pointer
 *         (move-only RAII), plus a niche-optimized Option<Box<T, A>>.
 *
 * sizeof(Box<T>)         == sizeof(T*)   (GlobalAllocator is empty → EBO)
 * sizeof(Option<Box<T>>) == sizeof(T*)   ← matches Rust's Option<Box<T>>
 *
 * For stateful Allocs, sizeof grows by sizeof(Allocator) (rounded for
 * alignment), matching std::unique_ptr's storage strategy.
 *
 * The "moved-from" state of Box technically holds a null pointer,
 * violating the public invariant. This is treated as a private
 * implementation detail visible only to the destructor (which guards
 * on null). Calling get() / operator* / operator-> on a moved-from
 * value is undefined — see std::unique_ptr's analogous contract.
 *
 * C++11-compatible. Header-only. No reset() — the type cannot be null.
 */

#ifndef XPP_BOX_H
#define XPP_BOX_H

#include <type_traits>
#include <utility>

#include <xpp/allocator.h>
#include <xpp/compressed_pair.h>
#include <xpp/nonnull.h>
#include <xpp/option.h>
#include <xpp/panic.h>

namespace xpp {

/**
 * @brief A non-null owning pointer to T with a custom Allocator.
 *
 * Semantically equivalent to a std::unique_ptr<T, D> that is never null.
 * Move-only. No reset (would imply nullable storage).
 *
 * Construction:
 *   - from_raw(T*, A = A{})       — mirrors unsafe Box::from_raw (debug-checked)
 *   - try_from_raw(T*, A = A{})   — checked, returns Option<Box>
 *
 * Also supports Box::into_raw() (consuming), mirroring Rust's Box::into_raw.
 *
 * @tparam T     Pointee type. T = void supported (operator*, ->
 *               SFINAE-removed).
 * @tparam Allocator Allocator used for deallocation. Defaults to
 *               GlobalAllocator (empty, EBO → sizeof(Box<T>) == sizeof(T*)).
 *               The Allocator is stored via CompressedPair with EBO when empty.
 */
template <class T, class Allocator = GlobalAllocator> class Box {
  using Storage = _::CompressedPair<T *, Allocator>;

public:
  using element_type   = T;
  using allocator_type = Allocator;
  using pointer        = T *;

  Box()                       = delete;
  Box(const Box &)            = delete;
  Box &operator=(const Box &) = delete;

  /** @brief Move ctor. Source becomes a "destruction-only" husk. */
  Box(Box &&o) noexcept : m_storage(o.m_storage.first(), std::move(o.m_storage.second())) {
    o.m_storage.first() = nullptr;
  }

  /**
   * @brief Covariant move ctor: Box<U, Allocator> → Box<T, Allocator>.
   *
   * Same Allocator required — different Allocs would have different
   * CompressedPair layouts. Enabled when U* is convertible to T*.
   */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Box(Box<U, Allocator> &&o) noexcept
      : m_storage(static_cast<T *>(o.m_storage.first()),
                  static_cast<Allocator>(std::move(o.m_storage.second()))) {
    o.m_storage.first() = nullptr;
  }

  /** @brief Move assignment. Old target is destroyed; source becomes husk. */
  Box &operator=(Box &&o) noexcept {
    if (this != &o) {
      reset_internal();
      m_storage.first()   = o.m_storage.first();
      m_storage.second()  = std::move(o.m_storage.second());
      o.m_storage.first() = nullptr;
    }
    return *this;
  }

  ~Box() {
    reset_internal();
  }

  /** @brief Wrap a raw pointer; caller asserts non-null (mirrors unsafe Box::from_raw). */
  static Box from_raw(T *p, Allocator a = Allocator{}) noexcept {
    XPP_DEBUG_ASSERT(p != nullptr, "Box::from_raw: pointer is null");
    return Box(p, std::move(a), _PrivateTag{});
  }

  /** @brief Checked factory. Returns None if p is null. */
  static Option<Box> try_from_raw(T *p, Allocator a = Allocator{}) noexcept;

  T *get() const noexcept {
    return m_storage.first();
  }

  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U &operator*() const noexcept {
    return *m_storage.first();
  }

  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U *operator->() const noexcept {
    return m_storage.first();
  }

  Allocator &allocator() noexcept {
    return m_storage.second();
  }
  const Allocator &allocator() const noexcept {
    return m_storage.second();
  }

  /** @brief Borrow as a non-owning NonNull view. */
  NonNull<T> as_nonnull() const noexcept {
    return NonNull<T>::new_unchecked(m_storage.first());
  }

  /**
   * @brief Relinquish ownership; return the raw pointer.
   *
   * Mirrors Box::into_raw. Consuming (rvalue-only) so we can
   * never leave a Box observable in a "null" state.
   */
  T *into_raw() && noexcept {
    T *r              = m_storage.first();
    m_storage.first() = nullptr;
    return r;
  }

private:
  struct _PrivateTag {};
  Box(T *p, Allocator a, _PrivateTag) noexcept : m_storage(p, std::move(a)) {}

  void reset_internal() noexcept {
    if (m_storage.first()) {
      _::destroy_and_dealloc(m_storage.first(), m_storage.second());
      m_storage.first() = nullptr;
    }
  }

  Storage m_storage;

  // Allow Option<Box<...>> (any instantiation) to access storage —
  // needed by both the matching specialization's take_owned() and by the
  // covariant ctor on Option<Box<Base>> reaching into a moved-from
  // Option<Box<Derived>>.
  template <class> friend class Option;
  // Allow covariant ctor to access another instantiation's storage.
  template <class, class> friend class Box;
  // Allow Own to reach into Box storage.
  template <class, class> friend class Own;
};

/**
 * @brief Niche-optimized Option<Box<T, A>>. Storage is the same
 *        pair as Box itself; nullptr ↔ None.
 *
 * Move-only (mirrors stored type). The destructor invokes the alloc
 * iff the storage pointer is non-null.
 *
 * unwrap() is asymmetric:
 *   - const &  → returns T* (a non-owning borrow). Cannot return owning
 *                Box (move-only) and cannot return a reference
 *                because no owning value lives in storage.
 *   - &&       → returns Box<T, A> by move (consumes).
 *
 * Combinators map() / and_then() / filter() / inspect():
 *   - const & overload  → fn receives NonNull<T> (non-owning view)
 *   - && overload       → fn receives Box<T, A>&& (consumes)
 */
template <class T, class Allocator> class Option<Box<T, Allocator>> {
  using Storage = _::CompressedPair<T *, Allocator>;

public:
  using value_type = Box<T, Allocator>;

  Option() noexcept : m_storage() {}
  Option(None) noexcept : m_storage() {}
  Option(Box<T, Allocator> &&u) noexcept
      : m_storage(u.m_storage.first(), std::move(u.m_storage.second())) {
    u.m_storage.first() = nullptr;
  }

  /** @brief Covariant: adopt Box<U, Allocator>. */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Option(Box<U, Allocator> &&u) noexcept
      : m_storage(static_cast<T *>(u.m_storage.first()),
                  static_cast<Allocator>(std::move(u.m_storage.second()))) {
    u.m_storage.first() = nullptr;
  }

  Option(const Option &)            = delete;
  Option &operator=(const Option &) = delete;

  Option(Option &&o) noexcept : m_storage(o.m_storage.first(), std::move(o.m_storage.second())) {
    o.m_storage.first() = nullptr;
  }

  /** @brief Covariant: adopt Option<Box<U, Allocator>>. */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Option(Option<Box<U, Allocator>> &&o) noexcept
      : m_storage(static_cast<T *>(o.m_storage.first()),
                  static_cast<Allocator>(std::move(o.m_storage.second()))) {
    o.m_storage.first() = nullptr;
  }
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      reset_internal();
      m_storage.first()   = o.m_storage.first();
      m_storage.second()  = std::move(o.m_storage.second());
      o.m_storage.first() = nullptr;
    }
    return *this;
  }
  Option &operator=(None) noexcept {
    reset_internal();
    return *this;
  }

  ~Option() {
    reset_internal();
  }

  bool is_some() const noexcept {
    return m_storage.first() != nullptr;
  }
  bool is_none() const noexcept {
    return m_storage.first() == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_storage.first() != nullptr;
  }

  /* ── unwrap (asymmetric: borrow vs consume) ─────────────────────── */

  T *unwrap() const & {
    XPP_ASSERT(m_storage.first() != nullptr, "unwrap() on None Option");
    return m_storage.first();
  }
  Box<T, Allocator> unwrap() && {
    XPP_ASSERT(m_storage.first() != nullptr, "unwrap() on None Option");
    return take_owned();
  }

  T *unwrap_unchecked() const & noexcept {
    XPP_DEBUG_ASSERT(m_storage.first(), "internal: Option must be Some");
    return m_storage.first();
  }
  Box<T, Allocator> unwrap_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_storage.first(), "internal: Option must be Some");
    return take_owned();
  }

  Box<T, Allocator> unwrap_or(Box<T, Allocator> &&fallback) && {
    if (m_storage.first()) return take_owned();
    return std::move(fallback);
  }

  Option take() noexcept {
    return std::move(*this);
  }

  T *expect(const char *msg) const & {
    XPP_ASSERT(m_storage.first() != nullptr, "expect: %s", msg);
    return m_storage.first();
  }
  Box<T, Allocator> expect(const char *msg) && {
    XPP_ASSERT(m_storage.first() != nullptr, "expect: %s", msg);
    return take_owned();
  }

  /* ── combinators ────────────────────────────────────────────────── */

  template <class Func>
  auto map(Func &&fn) const & -> Option<decltype(fn(std::declval<NonNull<T>>()))> {
    using U = decltype(fn(std::declval<NonNull<T>>()));
    return m_storage.first() ? Option<U>(fn(NonNull<T>::new_unchecked(m_storage.first())))
                             : Option<U>(none);
  }
  template <class Func>
  auto map(Func &&fn) && -> Option<decltype(fn(std::declval<Box<T, Allocator> &&>()))> {
    using U = decltype(fn(std::declval<Box<T, Allocator> &&>()));
    if (!m_storage.first()) return Option<U>(none);
    Box<T, Allocator> owned = take_owned();
    return Option<U>(fn(std::move(owned)));
  }

  template <class Func>
  auto and_then(Func &&fn) && -> decltype(fn(std::declval<Box<T, Allocator> &&>())) {
    using R = decltype(fn(std::declval<Box<T, Allocator> &&>()));
    if (!m_storage.first()) return R(none);
    Box<T, Allocator> owned = take_owned();
    return fn(std::move(owned));
  }

  template <class Func> Option or_else(Func &&fn) && {
    if (m_storage.first()) return std::move(*this);
    return fn();
  }

  template <class Func> Box<T, Allocator> unwrap_or_else(Func &&fn) && {
    if (m_storage.first()) return take_owned();
    return fn();
  }

  template <class Func> Option filter(Func &&pred) && {
    if (m_storage.first() && pred(NonNull<T>::new_unchecked(m_storage.first()))) {
      return std::move(*this);
    }
    reset_internal();
    return none;
  }

  template <class Func> const Option &inspect(Func &&fn) const & {
    if (m_storage.first()) fn(NonNull<T>::new_unchecked(m_storage.first()));
    return *this;
  }
  template <class Func> Option inspect(Func &&fn) && {
    if (m_storage.first()) fn(NonNull<T>::new_unchecked(m_storage.first()));
    return std::move(*this);
  }

private:
  void reset_internal() noexcept {
    if (m_storage.first()) {
      _::destroy_and_dealloc(m_storage.first(), m_storage.second());
      m_storage.first() = nullptr;
    }
  }

  /** Move ownership out of storage; storage left empty. Caller has checked p != null. */
  Box<T, Allocator> take_owned() noexcept {
    Box<T, Allocator> r =
      Box<T, Allocator>::from_raw(m_storage.first(), std::move(m_storage.second()));
    m_storage.first() = nullptr;
    return r;
  }

  Storage m_storage;

  // Allow covariant ctor to access another instantiation's storage.
  template <class> friend class Option;
};

/* ── Box<T, A>::from definition ─────────────────────────────── */

template <class T, class Allocator>
inline Option<Box<T, Allocator>> Box<T, Allocator>::try_from_raw(T *p, Allocator a) noexcept {
  if (!p) return Option<Box<T, Allocator>>(none);
  return Option<Box<T, Allocator>>(Box<T, Allocator>(p, std::move(a), _PrivateTag{}));
}

/* ── Compile-time size guarantees ────────────────────────────────────── */

static_assert(sizeof(Box<int>) == sizeof(int *),
              "Box<T, GlobalAllocator> must be sizeof(T*) via EBO");
static_assert(sizeof(Option<Box<int>>) == sizeof(int *),
              "Option<Box<T, GlobalAllocator>> niche broken");

} // namespace xpp

#endif // XPP_BOX_H
