/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * rc.h - Rc<T, Allocator>: a non-null, shared-owning, reference-counted
 *        handle to a heap-allocated T. Plus a niche-optimized
 *        Option<Rc<T, Allocator>> at sizeof(T*).
 *
 *   sizeof(Rc<T, A>)         == sizeof(T*)   (Allocator lives in RcInner, not Rc)
 *   sizeof(Option<Rc<T, A>>) == sizeof(T*)   (niche: nullptr ↔ None)
 *
 * Design analogue: Rust's std::rc::Rc<T, A> + Option<Rc<T, A>>. Like
 * Rust's Rc, libx++'s Rc is **co-located but NOT intrusive**: T does
 * not need to inherit from anything. Rc<T, Allocator>::make(args...) is
 * the only construction entry — the inner block carries the strong
 * and weak counts next to T, so the whole thing is a single heap
 * allocation.
 *
 * The name "Rc" is deliberate. C++ already has "Ref" overloaded
 * across libraries (WebKit's WTF::Ref is intrusive; Qt's QRef is yet
 * another flavour); the Rc spelling makes it clear which design this
 * one follows, even to readers who haven't read the header.
 *
 * Allocator:
 *   Rc<T, Allocator> stores the Allocator instance inside RcInner<T, Allocator>
 *   (with EBO when Allocator is empty), NOT inside the Rc handle itself.
 *   This keeps sizeof(Rc<T, A>) == sizeof(T*) regardless of A. The
 *   allocator is moved out of RcInner before deallocation so it can
 *   be used to free the very memory that contained it.
 *
 *   - `Rc<T>::make(args...)`              — uses default GlobalAllocator{}
 *   - `Rc<T, MyAlloc>::make(args...)`     — uses default-constructed MyAlloc
 *   - `Rc<T, MyAlloc>::make(alloc, args...)` — uses provided alloc instance
 *   - `Rc<T, MyAlloc>::make_in(alloc, args...)` — explicit (no SFINAE)
 *
 * Choose Rc<T> when:
 *   - Ownership is shared (the same T is reachable from several places).
 *   - You want sizeof == sizeof(T*) per handle, not the 2-word
 *     std::shared_ptr layout.
 *   - Single-thread. Cross-thread sharing is undefined behaviour;
 *     use Arc<T> (see xpp/arc.h) for that.
 *
 * Choose Own<T> instead when ownership is unique.
 * Choose NonNull<T> / Option<NonNull<T>> for non-owning references.
 *
 * Cycle breaking:
 *   Reference cycles built out of Rcs alone will leak. Break them
 *   with Weak<T> on the back-edge (see xpp/weak.h). Rust does the
 *   same trick.
 *
 * Reference-count layout (matches Rust):
 *
 *   strong = number of Rc<T>      pointing at this inner
 *   weak   = number of Weak<T>    pointing at this inner, **plus one**
 *            for the set of all live Rc<T>s. That extra +1 means we
 *            never need to special-case "any strong alive?" — when
 *            the last Rc drops it decrements weak normally, and the
 *            inner is deallocated iff weak hits zero. T's destructor
 *            runs at the strong→0 transition; the inner's memory is
 *            freed at the weak→0 transition. Those two events can be
 *            the same call when no Weak ever existed.
 *
 * Thread safety:
 *   Rc<T> is **single-thread**. Both counts are plain size_t and
 *   sharing a single Rc<T> (or its Weak<T>) across threads is UB.
 *   For thread-safe shared ownership use Arc<T> in xpp/arc.h.
 *
 * C++11-compatible. Header-only. Trivially nothing — Rc carries
 * non-trivial copy/destroy semantics that ref-count the inner.
 */

#ifndef XPP_RC_H
#define XPP_RC_H

#include <cstddef>
#include <type_traits>
#include <utility>

#include <xpp/allocator.h>
#include <xpp/option.h>
#include <xpp/panic.h>

namespace xpp {

template <class T, class Allocator = GlobalAllocator> class Rc;
template <class T, class Allocator = GlobalAllocator> class Weak;

namespace _ {

/**
 * @brief Heap-allocated control block + value, the storage Rc<T>
 *        and Weak<T> share.
 *
 * Layout (non-EBO): { strong, weak, value, alloc }
 * Layout (EBO):     inherits Allocator, then { strong, weak, value }
 *
 * In both cases strong is at offset 0, weak at offset 8, value at
 * offset 16 — so covariant reinterpret_casts between RcInner<T, A>
 * and RcInner<U, A> (for T/U related by inheritance) are valid.
 *
 * Lifecycle:
 *   - Rc<T, Allocator>::make(...) allocates the inner with strong=1, weak=1.
 *   - Each new Rc<T> bumps strong; each new Weak<T> bumps weak.
 *   - The last Rc to drop destroys T in place (~value), then
 *     decrements weak as if it were a Weak (the "all-strongs-count-
 *     as-one-weak" trick).
 *   - The last Weak to drop deallocates the inner block.
 *
 * `strong` and `weak` are plain size_t — Rc is single-thread. Arc
 * has its own ArcInner with std::atomic<size_t> in xpp/arc.h.
 */
template <class T, class Allocator,
          bool UseEbo = std::is_empty<Allocator>::value && !is_final<Allocator>::value>
struct RcInner {
  size_t    strong;
  size_t    weak;
  T         value;
  Allocator alloc;

  template <class A, class... Args>
  explicit RcInner(A &&a, Args &&...args)
      : strong(1), weak(1), value(std::forward<Args>(args)...), alloc(std::forward<A>(a)) {}

  Allocator &alloc_ref() noexcept {
    return alloc;
  }
};

template <class T, class Allocator> struct RcInner<T, Allocator, true> : private Allocator {
  size_t strong;
  size_t weak;
  T      value;

  template <class A, class... Args>
  explicit RcInner(A &&a, Args &&...args)
      : Allocator(std::forward<A>(a)), strong(1), weak(1), value(std::forward<Args>(args)...) {}

  Allocator &alloc_ref() noexcept {
    return static_cast<Allocator &>(*this);
  }
};

/**
 * @brief Drop the +1 on the "all strongs" weak side and possibly
 *        deallocate the inner block.
 *
 * Called twice in the lifecycle: once when a Weak drops, once when
 * the last Rc drops (after destroying T). Factored out so both call
 * sites use exactly the same end-of-life decision.
 *
 * When weak hits 0: move the Allocator out of the inner (so it survives
 * the deallocate call), then dealloc the inner's memory. T was
 * already destroyed by rc_dec_strong on the strong→0 transition.
 */
template <class T, class Allocator>
inline void rc_dec_weak_and_maybe_dealloc(RcInner<T, Allocator> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: rc_dec_weak called with null inner");
  if (--inner->weak == 0) {
    // Move alloc out before deallocating the memory that contains it.
    // For empty Allocs (the common case) this is a no-op.
    Allocator a      = std::move(inner->alloc_ref());
    Layout    layout = Layout::of<RcInner<T, Allocator>>();
    a.deallocate(inner, layout);
  }
}

/**
 * @brief Drop a strong reference. Destroy T in place when the last
 *        strong leaves, then decrement weak (the +1 for "any strong
 *        alive"). The inner block itself dies at weak->0, possibly
 *        in a different call if a Weak is still observing.
 */
template <class T, class Allocator>
inline void rc_dec_strong(RcInner<T, Allocator> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: rc_dec_strong called with null inner");
  if (--inner->strong == 0) {
    inner->value.~T();
    rc_dec_weak_and_maybe_dealloc(inner);
  }
}

} // namespace _

/**
 * @brief Non-null shared-owning reference-counted handle to a
 *        heap-allocated T.
 *
 * Always points at a live T whose RcInner has strong >= 1. Move-from
 * leaves the source in an unspecified state (internally null) —
 * using the moved-from Rc is undefined; only destruction is safe.
 * Same contract as std::unique_ptr's moved-from state.
 *
 * Use Rc<T, Allocator>::make(args...) to construct. There is no public
 * constructor from a raw T* — Rc always owns the allocation it was
 * born with, and a stray pointer can't be retro-fitted with a count.
 *
 * @tparam T     Pointee type.
 * @tparam Allocator Allocator used for the RcInner block. Defaults to
 *               GlobalAllocator (empty, EBO → zero overhead). The
 *               Allocator instance lives inside RcInner, not inside Rc,
 *               so sizeof(Rc<T, Allocator>) == sizeof(T*) for any Allocator.
 */
template <class T, class Allocator> class Rc {
  static_assert(std::is_move_constructible<Allocator>::value,
                "Allocator must be move-constructible to support deallocation");

public:
  using value_type     = T;
  using allocator_type = Allocator;

  /**
   * @brief Copy constructor: +1 on the shared strong count.
   *
   * Implicit on purpose. For hot-path code where "this is a new
   * owner" should be visually obvious, call `.clone()` or the
   * Rust-style `Rc<T>::clone(&r)` static helper instead — both do
   * the same thing as copy construction but read as deliberate.
   */
  Rc(const Rc &o) noexcept : m_inner(o.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    ++m_inner->strong;
  }

  /**
   * @brief Move constructor: zero count changes; the source becomes
   *        invalid (UB to use other than to destroy).
   */
  Rc(Rc &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  /**
   * @brief Covariant copy: Rc<U, Allocator> → Rc<T, Allocator>.
   *
   * Same Allocator required — different Allocs would have different
   * RcInner layouts, breaking the reinterpret_cast.
   */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Rc(const Rc<U, Allocator> &o) noexcept
      : m_inner(reinterpret_cast<_::RcInner<T, Allocator> *>(o.inner_raw())) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    ++m_inner->strong;
  }

  /**
   * @brief Covariant move: Rc<U, Allocator> → Rc<T, Allocator>.
   */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Rc(Rc<U, Allocator> &&o) noexcept
      : m_inner(reinterpret_cast<_::RcInner<T, Allocator> *>(o.inner_raw())) {
    o.inner_raw_reset();
  }

  Rc &operator=(const Rc &o) noexcept {
    if (this != &o) {
      Rc tmp(o);
      swap(tmp);
    }
    return *this;
  }

  Rc &operator=(Rc &&o) noexcept {
    if (this != &o) {
      Rc tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~Rc() noexcept {
    if (m_inner) _::rc_dec_strong(m_inner);
  }

  /**
   * @brief Explicit +1, member-style. See also the static
   *        Rc<T>::clone(&r) for the Rust calling convention.
   */
  Rc clone() const noexcept {
    return Rc(*this);
  }

  /**
   * @brief Strong reference count (debugging only).
   *
   * Don't branch on this in production code — a same-thread clone
   * can change it between the read and any decision you make. Mostly
   * useful for assertions and tests ("did this drop?").
   */
  size_t strong_count() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return m_inner->strong;
  }

  /**
   * @brief Weak reference count (debugging only).
   *
   * Counts the live Weak<T>s observing this inner. The "all strongs
   * count as one weak" +1 is NOT included — when only Rcs exist this
   * reads 0, matching Rust's `Rc::weak_count`.
   */
  size_t weak_count() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return m_inner->weak - 1;
  }

  T &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return m_inner->value;
  }
  T *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return &m_inner->value;
  }

  T *get() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return &m_inner->value;
  }

  void swap(Rc &o) noexcept {
    _::RcInner<T, Allocator> *tmp = m_inner;
    m_inner                       = o.m_inner;
    o.m_inner                     = tmp;
  }

  bool operator==(const Rc &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const Rc &o) const noexcept {
    return m_inner != o.m_inner;
  }

  /**
   * @brief Rust-style explicit +1: `Rc<T>::clone(&r)`.
   *
   * Static method that takes a `const Rc<T>&` and returns a new
   * Rc<T>. Same semantics as the member `.clone()` and copy
   * construction.
   */
  static Rc clone(const Rc &r) noexcept {
    return r.clone();
  }

  /**
   * @brief Drop a strong reference, return a non-owning Weak.
   *
   * Same as `Weak<T, Allocator>(r)`. Rust spells this `Rc::downgrade(&r)`. The
   * caller still owns the original strong reference; the new Weak
   * adds +1 to the weak count.
   */
  static Weak<T, Allocator> downgrade(const Rc &r) noexcept;

  /* ── internals exposed only to friends / templates ───────────────── */

  // Used by covariant ctor of Rc<Other>; do not call directly.
  _::RcInner<T, Allocator> *inner_raw() const noexcept {
    return m_inner;
  }
  void inner_raw_reset() noexcept {
    m_inner = nullptr;
  }

private:
  // Constructed only by Rc::make, the friend Option specialization,
  // and Weak<T, Allocator>::upgrade. m_inner is always non-null at this point.
  explicit Rc(_::RcInner<T, Allocator> *inner) noexcept : m_inner(inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
  }

  _::RcInner<T, Allocator> *m_inner;

  template <class, class> friend class Rc;
  template <class, class> friend class Weak;
  friend class Option<Rc<T, Allocator>>;

public:
  /* ── factories ──────────────────────────────────────────────────── */

  /**
   * @brief Construct an Rc<T, Allocator> in place using a default-constructed
   *        Allocator.
   *
   * Single heap allocation: the inner block holds the strong count
   * (=1), the weak count (=1, the implicit "all strongs are one weak"
   * marker), and a freshly-constructed T.
   *
   * If the first argument is convertible to `Allocator`, it is treated as
   * the allocator instance (for stateful allocators); otherwise, the
   * Allocator is default-constructed and all arguments are forwarded to T's
   * constructor. For the ambiguous case (T's first ctor arg is
   * convertible to Allocator), use `make_in` explicitly.
   *
   * libx++ does not use exceptions for control flow (see README), so
   * make expects T's constructor not to throw. If T can fail to
   * construct, model that with a static factory returning
   * Result<T, Error> and call make on the unwrapped success path.
   */
  template <class... Args,
            class = typename std::enable_if<!_::FirstIsAlloc<Allocator, Args...>::value>::type>
  static Rc make(Args &&...args) {
    return make_in(Allocator{}, std::forward<Args>(args)...);
  }

  /** @brief Overload that accepts an Allocator instance as the first argument. */
  template <class First, class... Rest,
            class = typename std::enable_if<
              std::is_convertible<typename std::decay<First>::type, Allocator>::value>::type>
  static Rc make(First &&alloc, Rest &&...rest) {
    return make_in(std::forward<First>(alloc), std::forward<Rest>(rest)...);
  }

  /**
   * @brief Construct an Rc<T, Allocator> with an explicit allocator instance.
   *
   * Use this when `make`'s SFINAE detection is ambiguous (T's first
   * ctor arg is convertible to Allocator) or when you want to be explicit
   * about which argument is the allocator.
   */
  template <class AllocArg, class... Args> static Rc make_in(AllocArg &&alloc, Args &&...args) {
    Layout layout = Layout::of<_::RcInner<T, Allocator>>();
    auto   r      = alloc.allocate(layout);
    XPP_ASSERT(r.is_ok(), "Rc::make_in: allocation failed");
    void                     *mem   = r.unwrap().data();
    _::RcInner<T, Allocator> *inner = ::new (mem)
      _::RcInner<T, Allocator>(std::forward<AllocArg>(alloc), std::forward<Args>(args)...);
    return Rc(inner);
  }
};

template <class T, class Allocator> void swap(Rc<T, Allocator> &a, Rc<T, Allocator> &b) noexcept {
  a.swap(b);
}

/**
 * @brief Niche-optimized Option<Rc<T, Allocator>>. Storage is a single
 *        RcInner<T, Allocator>* whose nullptr value means None.
 *
 *   sizeof(Option<Rc<T, Allocator>>) == sizeof(T*)
 *
 * Works for any Allocator because sizeof(Rc<T, Allocator>) == sizeof(T*)
 * (the Allocator lives in RcInner, not in the Rc handle).
 *
 * Owns a +1 on the strong count whenever it is Some, releases it on
 * destruction or when overwritten with None.
 */
template <class T, class Allocator> class Option<Rc<T, Allocator>> {
public:
  using value_type = Rc<T, Allocator>;

  constexpr Option() noexcept : m_inner(nullptr) {}
  constexpr Option(None) noexcept : m_inner(nullptr) {}

  Option(const Rc<T, Allocator> &r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    ++m_inner->strong;
  }
  Option(Rc<T, Allocator> &&r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    r.m_inner = nullptr;
  }

  Option(const Option &o) noexcept : m_inner(o.m_inner) {
    if (m_inner) ++m_inner->strong;
  }
  Option(Option &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  Option &operator=(const Option &o) noexcept {
    if (this != &o) {
      Option tmp(o);
      swap(tmp);
    }
    return *this;
  }
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      Option tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }
  Option &operator=(None) noexcept {
    Option tmp;
    swap(tmp);
    return *this;
  }

  ~Option() noexcept {
    if (m_inner) _::rc_dec_strong(m_inner);
  }

  bool is_some() const noexcept {
    return m_inner != nullptr;
  }
  bool is_none() const noexcept {
    return m_inner == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_inner != nullptr;
  }

  Rc<T, Allocator> unwrap() && {
    XPP_ASSERT(m_inner != nullptr, "unwrap() on None Option");
    _::RcInner<T, Allocator> *taken = m_inner;
    m_inner                         = nullptr;
    return Rc<T, Allocator>(taken);
  }

  Rc<T, Allocator> unwrap_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Option must be Some");
    _::RcInner<T, Allocator> *taken = m_inner;
    m_inner                         = nullptr;
    return Rc<T, Allocator>(taken);
  }

  Rc<T, Allocator> take() {
    return std::move(*this).unwrap();
  }

  void swap(Option &o) noexcept {
    _::RcInner<T, Allocator> *tmp = m_inner;
    m_inner                       = o.m_inner;
    o.m_inner                     = tmp;
  }

private:
  _::RcInner<T, Allocator> *m_inner;

  friend class Weak<T, Allocator>; // for Weak::upgrade() to construct directly
};

template <class T, class Allocator>
void swap(Option<Rc<T, Allocator>> &a, Option<Rc<T, Allocator>> &b) noexcept {
  a.swap(b);
}

/* ── invariants pinned at compile time ────────────────────────────── */

static_assert(sizeof(Rc<int>) == sizeof(int *), "Rc<T> must be sizeof(T*)");
static_assert(sizeof(Option<Rc<int>>) == sizeof(int *), "Option<Rc<T>> niche optimisation broken");

} // namespace xpp

#endif // XPP_RC_H
