/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * box.h - Box<T, Alloc>: a guaranteed-non-null owning smart pointer
 *         (move-only RAII), plus a niche-optimized Option<Box<T, A>>.
 *
 * sizeof(Box<T>)         == sizeof(T*)   (GlobalAllocator is empty → EBO)
 * sizeof(Option<Box<T>>) == sizeof(T*)   ← matches Rust's Option<Box<T>>
 *
 * For stateful Allocs, sizeof grows by sizeof(Alloc) (rounded for
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
#include <xpp/nonnull.h>
#include <xpp/option.h>
#include <xpp/panic.h>

namespace xpp {
namespace _ {

/**
 * @brief T* + Alloc pair with empty-base optimization for empty Allocs.
 *
 * Two specializations keyed on `is_empty<A> && !is_final<A>`:
 *   - empty + non-final → inherit privately from A, sizeof == sizeof(T*)
 *   - otherwise         → store A as a member, sizeof == sizeof(T*) + sizeof(A)
 *
 * Mirrors the strategy used by libc++ / libstdc++ / MSVC STL inside
 * std::unique_ptr.
 */
template <class T, class A, bool Empty = std::is_empty<A>::value && !IsFinal<A>::value>
struct CompressedPair {
  T *p;
  A  a;

  CompressedPair() noexcept(std::is_nothrow_default_constructible<A>::value) : p(nullptr), a() {}
  CompressedPair(T *p_, A a_) noexcept(std::is_nothrow_move_constructible<A>::value)
      : p(p_), a(std::move(a_)) {}

  A &allocator() noexcept {
    return a;
  }
  const A &allocator() const noexcept {
    return a;
  }
};

template <class T, class A> struct CompressedPair<T, A, true> : private A {
  T *p;

  CompressedPair() noexcept(std::is_nothrow_default_constructible<A>::value) : A(), p(nullptr) {}
  CompressedPair(T *p_, A a_) noexcept(std::is_nothrow_move_constructible<A>::value)
      : A(std::move(a_)), p(p_) {}

  A &allocator() noexcept {
    return *this;
  }
  const A &allocator() const noexcept {
    return *this;
  }
};

} // namespace _

/**
 * @brief A non-null owning pointer to T with a custom Alloc.
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
 * @tparam Alloc Allocator used for deallocation. Defaults to
 *               GlobalAllocator (empty, EBO → sizeof(Box<T>) == sizeof(T*)).
 *               The Alloc is stored via CompressedPair with EBO when empty.
 */
template <class T, class Alloc = GlobalAllocator> class Box {
  using Storage = _::CompressedPair<T, Alloc>;

public:
  using element_type   = T;
  using allocator_type = Alloc;
  using pointer        = T *;

  Box()                       = delete;
  Box(const Box &)            = delete;
  Box &operator=(const Box &) = delete;

  /** @brief Move ctor. Source becomes a "destruction-only" husk. */
  Box(Box &&o) noexcept : m_storage(o.m_storage.p, std::move(o.m_storage.allocator())) {
    o.m_storage.p = nullptr;
  }

  /**
   * @brief Covariant move ctor: Box<U, Alloc> → Box<T, Alloc>.
   *
   * Same Alloc required — different Allocs would have different
   * CompressedPair layouts. Enabled when U* is convertible to T*.
   */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Box(Box<U, Alloc> &&o) noexcept
      : m_storage(static_cast<T *>(o.m_storage.p),
                  static_cast<Alloc>(std::move(o.m_storage.allocator()))) {
    o.m_storage.p = nullptr;
  }

  /** @brief Move assignment. Old target is destroyed; source becomes husk. */
  Box &operator=(Box &&o) noexcept {
    if (this != &o) {
      reset_internal();
      m_storage.p       = o.m_storage.p;
      m_storage.allocator() = std::move(o.m_storage.allocator());
      o.m_storage.p     = nullptr;
    }
    return *this;
  }

  ~Box() {
    reset_internal();
  }

  /** @brief Wrap a raw pointer; caller asserts non-null (mirrors unsafe Box::from_raw). */
  static Box from_raw(T *p, Alloc a = Alloc{}) noexcept {
    XPP_DEBUG_ASSERT(p != nullptr, "Box::from_raw: pointer is null");
    return Box(p, std::move(a), _PrivateTag{});
  }

  /** @brief Checked factory. Returns None if p is null. */
  static Option<Box> try_from_raw(T *p, Alloc a = Alloc{}) noexcept;

  T *get() const noexcept {
    return m_storage.p;
  }

  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U &operator*() const noexcept {
    return *m_storage.p;
  }

  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U *operator->() const noexcept {
    return m_storage.p;
  }

  Alloc &allocator() noexcept {
    return m_storage.allocator();
  }
  const Alloc &allocator() const noexcept {
    return m_storage.allocator();
  }

  /** @brief Borrow as a non-owning NonNull view. */
  NonNull<T> as_nonnull() const noexcept {
    return NonNull<T>::new_unchecked(m_storage.p);
  }

  /**
   * @brief Relinquish ownership; return the raw pointer.
   *
   * Mirrors Box::into_raw. Consuming (rvalue-only) so we can
   * never leave a Box observable in a "null" state.
   */
  T *into_raw() && noexcept {
    T *r        = m_storage.p;
    m_storage.p = nullptr;
    return r;
  }

private:
  struct _PrivateTag {};
  Box(T *p, Alloc a, _PrivateTag) noexcept : m_storage(p, std::move(a)) {}

  void reset_internal() noexcept {
    if (m_storage.p) {
      _::destroy_and_dealloc(m_storage.p, m_storage.allocator());
      m_storage.p = nullptr;
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
template <class T, class Alloc> class Option<Box<T, Alloc>> {
  using Storage = _::CompressedPair<T, Alloc>;

public:
  using value_type = Box<T, Alloc>;

  Option() noexcept : m_storage() {}
  Option(None) noexcept : m_storage() {}
  Option(Box<T, Alloc> &&u) noexcept : m_storage(u.m_storage.p, std::move(u.m_storage.allocator())) {
    u.m_storage.p = nullptr;
  }

  /** @brief Covariant: adopt Box<U, Alloc>. */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Option(Box<U, Alloc> &&u) noexcept
      : m_storage(static_cast<T *>(u.m_storage.p),
                  static_cast<Alloc>(std::move(u.m_storage.allocator()))) {
    u.m_storage.p = nullptr;
  }

  Option(const Option &)            = delete;
  Option &operator=(const Option &) = delete;

  Option(Option &&o) noexcept : m_storage(o.m_storage.p, std::move(o.m_storage.allocator())) {
    o.m_storage.p = nullptr;
  }

  /** @brief Covariant: adopt Option<Box<U, Alloc>>. */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value &&
                                                     !std::is_same<U, T>::value>::type>
  Option(Option<Box<U, Alloc>> &&o) noexcept
      : m_storage(static_cast<T *>(o.m_storage.p),
                  static_cast<Alloc>(std::move(o.m_storage.alloc()))) {
    o.m_storage.p = nullptr;
  }
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      reset_internal();
      m_storage.p       = o.m_storage.p;
      m_storage.allocator() = std::move(o.m_storage.allocator());
      o.m_storage.p     = nullptr;
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
    return m_storage.p != nullptr;
  }
  bool is_none() const noexcept {
    return m_storage.p == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_storage.p != nullptr;
  }

  /* ── unwrap (asymmetric: borrow vs consume) ─────────────────────── */

  T *unwrap() const & {
    XPP_ASSERT(m_storage.p != nullptr, "unwrap() on None Option");
    return m_storage.p;
  }
  Box<T, Alloc> unwrap() && {
    XPP_ASSERT(m_storage.p != nullptr, "unwrap() on None Option");
    return take_owned();
  }

  T *unwrap_unchecked() const & noexcept {
    XPP_DEBUG_ASSERT(m_storage.p, "internal: Option must be Some");
    return m_storage.p;
  }
  Box<T, Alloc> unwrap_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_storage.p, "internal: Option must be Some");
    return take_owned();
  }

  Box<T, Alloc> unwrap_or(Box<T, Alloc> &&fallback) && {
    if (m_storage.p) return take_owned();
    return std::move(fallback);
  }

  Option take() noexcept {
    return std::move(*this);
  }

  T *expect(const char *msg) const & {
    XPP_ASSERT(m_storage.p != nullptr, "expect: %s", msg);
    return m_storage.p;
  }
  Box<T, Alloc> expect(const char *msg) && {
    XPP_ASSERT(m_storage.p != nullptr, "expect: %s", msg);
    return take_owned();
  }

  /* ── combinators ────────────────────────────────────────────────── */

  template <class Func>
  auto map(Func &&fn) const & -> Option<decltype(fn(std::declval<NonNull<T>>()))> {
    using U = decltype(fn(std::declval<NonNull<T>>()));
    return m_storage.p ? Option<U>(fn(NonNull<T>::new_unchecked(m_storage.p))) : Option<U>(none);
  }
  template <class Func>
  auto map(Func &&fn) && -> Option<decltype(fn(std::declval<Box<T, Alloc> &&>()))> {
    using U = decltype(fn(std::declval<Box<T, Alloc> &&>()));
    if (!m_storage.p) return Option<U>(none);
    Box<T, Alloc> owned = take_owned();
    return Option<U>(fn(std::move(owned)));
  }

  template <class Func>
  auto and_then(Func &&fn) && -> decltype(fn(std::declval<Box<T, Alloc> &&>())) {
    using R = decltype(fn(std::declval<Box<T, Alloc> &&>()));
    if (!m_storage.p) return R(none);
    Box<T, Alloc> owned = take_owned();
    return fn(std::move(owned));
  }

  template <class Func> Option or_else(Func &&fn) && {
    if (m_storage.p) return std::move(*this);
    return fn();
  }

  template <class Func> Box<T, Alloc> unwrap_or_else(Func &&fn) && {
    if (m_storage.p) return take_owned();
    return fn();
  }

  template <class Func> Option filter(Func &&pred) && {
    if (m_storage.p && pred(NonNull<T>::new_unchecked(m_storage.p))) {
      return std::move(*this);
    }
    reset_internal();
    return none;
  }

  template <class Func> const Option &inspect(Func &&fn) const & {
    if (m_storage.p) fn(NonNull<T>::new_unchecked(m_storage.p));
    return *this;
  }
  template <class Func> Option inspect(Func &&fn) && {
    if (m_storage.p) fn(NonNull<T>::new_unchecked(m_storage.p));
    return std::move(*this);
  }

private:
  void reset_internal() noexcept {
    if (m_storage.p) {
      _::destroy_and_dealloc(m_storage.p, m_storage.allocator());
      m_storage.p = nullptr;
    }
  }

  /** Move ownership out of storage; storage left empty. Caller has checked p != null. */
  Box<T, Alloc> take_owned() noexcept {
    Box<T, Alloc> r = Box<T, Alloc>::from_raw(m_storage.p, std::move(m_storage.allocator()));
    m_storage.p     = nullptr;
    return r;
  }

  Storage m_storage;

  // Allow covariant ctor to access another instantiation's storage.
  template <class> friend class Option;
};

/* ── Box<T, A>::from definition ─────────────────────────────── */

template <class T, class Alloc>
inline Option<Box<T, Alloc>> Box<T, Alloc>::try_from_raw(T *p, Alloc a) noexcept {
  if (!p) return Option<Box<T, Alloc>>(none);
  return Option<Box<T, Alloc>>(Box<T, Alloc>(p, std::move(a), _PrivateTag{}));
}

/* ── Compile-time size guarantees ────────────────────────────────────── */

static_assert(sizeof(Box<int>) == sizeof(int *),
              "Box<T, GlobalAllocator> must be sizeof(T*) via EBO");
static_assert(sizeof(Option<Box<int>>) == sizeof(int *),
              "Option<Box<T, GlobalAllocator>> niche broken");

} // namespace xpp

#endif // XPP_BOX_H
