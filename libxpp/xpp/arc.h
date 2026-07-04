/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arc.h - Arc<T, Alloc>: the atomic, thread-safe counterpart of
 *         Rc<T, Alloc>. Plus ArcWeak<T, Alloc> and a niche-optimized
 *         Option<Arc<T, Alloc>> at sizeof(T*).
 *
 *   sizeof(Arc<T, A>)         == sizeof(T*)   (Alloc lives in ArcInner, not Arc)
 *   sizeof(Option<Arc<T, A>>) == sizeof(T*)   (niche: nullptr ↔ None)
 *   sizeof(ArcWeak<T, A>)     == sizeof(T*)
 *
 * Design analogue: Rust's std::sync::Arc<T, A> + Weak<T>. Same shape
 * as xpp::Rc<T, A> / xpp::Weak<T, A>, except the strong and weak
 * counts are std::atomic<size_t> and the operations use the carefully
 * chosen memory order pattern documented below.
 *
 * Rc vs Arc:
 *   - Rc<T, A> (xpp/rc.h): single-thread. clone/drop are plain ++/--.
 *   - Arc<T, A> (this file): cross-thread. clone/drop use atomic
 *     fetch_add / fetch_sub. About 3-5× slower than Rc clone on
 *     contended hardware, free on uncontended cache.
 *
 * Pick Rc for graphs that never cross a thread boundary; Arc only
 * when sharing across threads is real. Mixing the two for the same
 * object isn't supported (and isn't needed — one or the other
 * suffices for any given lifetime).
 *
 * Allocator:
 *   Arc<T, Alloc> stores the Alloc instance inside ArcInner<T, Alloc>
 *   (with EBO when Alloc is empty), NOT inside the Arc handle itself.
 *   This keeps sizeof(Arc<T, A>) == sizeof(T*) regardless of A. The
 *   allocator is moved out of ArcInner before deallocation so it can
 *   be used to free the very memory that contained it.
 *
 *   - `Arc<T>::make(args...)`              — uses default GlobalAllocator{}
 *   - `Arc<T, MyAlloc>::make(args...)`     — uses default-constructed MyAlloc
 *   - `Arc<T, MyAlloc>::make(alloc, args...)` — uses provided alloc instance
 *   - `Arc<T, MyAlloc>::make_in(alloc, args...)` — explicit (no SFINAE)
 *
 * Memory order discipline (matches Rust libstd / triomphe / boost):
 *
 *   strong clone (fetch_add(1)):   memory_order_relaxed
 *     The new owner does not synchronise with anything by virtue of
 *     incrementing; only side-effects through the value need
 *     ordering, and that's the caller's responsibility.
 *
 *   strong drop (fetch_sub(1)):    memory_order_release
 *     The dropping thread "releases" all writes it made to the
 *     pointee, so a thread that eventually destroys T can observe
 *     them after acquiring.
 *
 *   When strong hits 0:            atomic_thread_fence(acquire)
 *     Pair with the release above so the destruction sees writes
 *     from every previous owner. Cheaper than acq_rel on every
 *     drop — only the destroying thread pays.
 *
 *   weak clone (fetch_add(1)):     memory_order_relaxed
 *     Same reasoning as strong clone.
 *
 *   weak drop (fetch_sub(1)):      memory_order_release
 *     Pair with the acquire fence in the deallocator.
 *
 *   When weak hits 0:              atomic_thread_fence(acquire)
 *     Same pattern as for strong; the freeing thread acquires.
 *
 *   ArcWeak::upgrade:              CAS loop on strong
 *     Naïve "if strong > 0, ++strong" races with another thread's
 *     drop. Compare-exchange with relaxed-on-failure handles it.
 *
 * Reference-count layout (same as Rc):
 *
 *   strong = number of Arc<T>     pointing at this inner
 *   weak   = number of ArcWeak<T> + 1 for the set of all live Arcs
 *
 * ── Worked example: thread-safe observer registry ───────────────────
 *
 * ArcWeak<T> shines in publish/subscribe across threads: the
 * Publisher hands out callbacks that observe a Subscriber, but
 * holding the callback must NOT keep the Subscriber alive — once the
 * Subscriber's owner drops it, the callback should silently no-op.
 * Strong refs in both directions would deadlock the lifetime; Arc +
 * ArcWeak breaks the cycle the same way Rc/Weak does, but safely
 * across threads.
 *
 *   //  BAD — strong cycle, never freed (and across threads no less)
 *   struct Subscriber { … };
 *   struct Publisher {
 *     std::vector<Arc<Subscriber>>  subs;          // strong → subs
 *   };
 *   // Each Subscriber happens to also hold an Arc<Publisher> for
 *   // posting back; now publisher and subscriber pin each other
 *   // alive forever, and the leak survives every thread exiting.
 *
 *   //  GOOD — publisher holds ArcWeak, callbacks check at fire time
 *   struct Subscriber {
 *     void on_event(const Event &e);
 *   };
 *
 *   class Publisher {
 *   public:
 *     void subscribe(const Arc<Subscriber> &s) {
 *       std::lock_guard<std::mutex> lk(m_lock);
 *       m_subs.push_back(Arc<Subscriber>::downgrade(s));
 *     }
 *
 *     void publish(const Event &e) {
 *       std::lock_guard<std::mutex> lk(m_lock);
 *       // Walk the weak list; upgrade returns None for any
 *       // Subscriber whose last Arc has already dropped. Drop those
 *       // entries lazily during this same walk.
 *       auto write = m_subs.begin();
 *       for (auto read = m_subs.begin(); read != m_subs.end(); ++read) {
 *         Option<Arc<Subscriber>> live = read->upgrade();
 *         if (live.is_some()) {
 *           Arc<Subscriber> s = std::move(live).unwrap();
 *           s->on_event(e);                          // safe; we hold a strong
 *           *write++ = std::move(*read);
 *         }
 *         // else: subscriber already dropped, skip and don't copy.
 *       }
 *       m_subs.erase(write, m_subs.end());
 *     }
 *
 *   private:
 *     std::mutex                          m_lock;
 *     std::vector<ArcWeak<Subscriber>>    m_subs;
 *   };
 *
 *   // Producer thread:
 *   auto sub = Arc<Subscriber>::make();
 *   pub.subscribe(sub);
 *
 *   // Some other thread drops `sub` whenever it wants. Inside the
 *   // publisher, upgrade() races atomically with that drop:
 *   //   - If publish() arrives first, it bumps strong via the CAS,
 *   //     gets a valid Arc, and fires on_event. The Subscriber stays
 *   //     alive for the duration of the call.
 *   //   - If the last Arc drops first, upgrade() returns None and
 *   //     publish() skips the entry. No use-after-free either way.
 *
 * Rule of thumb (cross-thread variant of the Rc rule): the side
 * that **outlives** the subscription keeps Arc; the side that
 * **observes** keeps ArcWeak and upgrades on demand. Publisher
 * does NOT own its subscribers; subscribers own themselves (or are
 * owned by their domain).
 *
 * For single-thread cycle-breaking use Rc<T> + Weak<T> (xpp/rc.h,
 * xpp/weak.h).
 *
 * ────────��──────────────────────────────────────────────────────────
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_ARC_H
#define XPP_ARC_H

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

#include <xpp/allocator.h>
#include <xpp/option.h>
#include <xpp/panic.h>

namespace xpp {

template <class T, class Alloc = GlobalAllocator> class Arc;
template <class T, class Alloc = GlobalAllocator> class ArcWeak;

namespace _ {

/* ── ArcInner — control block + value, with EBO on empty Alloc ────── */
/**
 * @brief Atomic counterpart of RcInner. Same layout strategy, but
 *        strong/weak are std::atomic<size_t>. Stores the Alloc
 *        instance so it's available at deallocation time (last weak
 *        drop). Empty Allocs are EBO-eligible (inherit privately) →
 *        zero storage overhead.
 *
 * Layout (non-EBO): { strong, weak, value, alloc }
 * Layout (EBO):     inherits Alloc, then { strong, weak, value }
 *
 * In both cases strong is at offset 0, weak at offset 8, value at
 * offset 16 — so covariant reinterpret_casts between ArcInner<T, A>
 * and ArcInner<U, A> (for T/U related by inheritance) are valid.
 */
template <class T, class Alloc,
          bool UseEbo = std::is_empty<Alloc>::value && !is_final<Alloc>::value>
struct ArcInner {
  std::atomic<size_t> strong;
  std::atomic<size_t> weak;
  T                   value;
  Alloc               alloc;

  template <class A, class... Args>
  explicit ArcInner(A &&a, Args &&...args)
      : strong(1), weak(1), value(std::forward<Args>(args)...), alloc(std::forward<A>(a)) {}

  Alloc &alloc_ref() noexcept {
    return alloc;
  }
};

template <class T, class Alloc> struct ArcInner<T, Alloc, true> : private Alloc {
  std::atomic<size_t> strong;
  std::atomic<size_t> weak;
  T                   value;

  template <class A, class... Args>
  explicit ArcInner(A &&a, Args &&...args)
      : Alloc(std::forward<A>(a)), strong(1), weak(1), value(std::forward<Args>(args)...) {}

  Alloc &alloc_ref() noexcept {
    return static_cast<Alloc &>(*this);
  }
};

/**
 * @brief Drop a weak reference and possibly deallocate the inner.
 *
 * `release` on the decrement so the freeing thread (the one that
 * decrements to 0) can `acquire` and see every previous owner's
 * stores to the inner block.
 *
 * When weak hits 0: move the Alloc out of the inner (so it survives
 * the deallocate call), then dealloc the inner's memory. T was
 * already destroyed by arc_dec_strong on the strong→0 transition;
 * atomics are trivially destructible; the Alloc subobject is now in
 * a moved-from state and its resources are owned by the local `a`.
 */
template <class T, class Alloc>
inline void arc_dec_weak_and_maybe_dealloc(ArcInner<T, Alloc> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: arc_dec_weak called with null inner");
  if (inner->weak.fetch_sub(1, std::memory_order_release) == 1) {
    std::atomic_thread_fence(std::memory_order_acquire);
    // Move alloc out before deallocating the memory that contains it.
    // For empty Allocs (the common case) this is a no-op.
    Alloc  a      = std::move(inner->alloc_ref());
    Layout layout = Layout::of<ArcInner<T, Alloc>>();
    a.deallocate(inner, layout);
  }
}

/**
 * @brief Drop a strong reference. Same two-stage strategy as the
 *        Rc helper, but with the atomic ordering pattern above.
 */
template <class T, class Alloc> inline void arc_dec_strong(ArcInner<T, Alloc> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: arc_dec_strong called with null inner");
  if (inner->strong.fetch_sub(1, std::memory_order_release) == 1) {
    // Last strong gone. Acquire-fence to see every prior owner's
    // writes to *T*, then destroy T in place. The inner's memory
    // sticks around until the weak count also hits zero.
    std::atomic_thread_fence(std::memory_order_acquire);
    inner->value.~T();
    arc_dec_weak_and_maybe_dealloc(inner);
  }
}

} // namespace _

/**
 * @brief Non-null, thread-safe shared-owning reference-counted
 *        handle to a heap-allocated T.
 *
 * Identical semantics to Rc<T> except the counts are atomic and the
 * type is safe to clone/drop from multiple threads simultaneously.
 * Copy-construct on the consumer thread, drop on whichever thread
 * happens to own the last reference last — the destructor runs
 * exactly once and with a happens-before of every prior owner's
 * stores to T.
 *
 * Use Arc<T>::make(args...) to construct.
 *
 * @tparam T     Pointee type.
 * @tparam Alloc Allocator used for the ArcInner block. Defaults to
 *               GlobalAllocator (empty, EBO → zero overhead). The
 *               Alloc instance lives inside ArcInner, not inside Arc,
 *               so sizeof(Arc<T, Alloc>) == sizeof(T*) for any Alloc.
 */
template <class T, class Alloc> class Arc {
  static_assert(std::is_move_constructible<Alloc>::value,
                "Allocator must be move-constructible to support deallocation");

public:
  using value_type     = T;
  using allocator_type = Alloc;

  Arc(const Arc &o) noexcept : m_inner(o.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    // relaxed: a fresh owner does not synchronise with anything by
    // the fact of incrementing.
    m_inner->strong.fetch_add(1, std::memory_order_relaxed);
  }

  Arc(Arc &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  /**
   * @brief Covariant copy: Arc<U, Alloc> → Arc<T, Alloc>.
   *
   * Same Alloc required — different Allocs would have different
   * ArcInner layouts, breaking the reinterpret_cast.
   */
  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Arc(const Arc<U, Alloc> &o) noexcept
      : m_inner(reinterpret_cast<_::ArcInner<T, Alloc> *>(o.inner_raw())) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    m_inner->strong.fetch_add(1, std::memory_order_relaxed);
  }

  template <class U, class = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Arc(Arc<U, Alloc> &&o) noexcept
      : m_inner(reinterpret_cast<_::ArcInner<T, Alloc> *>(o.inner_raw())) {
    o.inner_raw_reset();
  }

  Arc &operator=(const Arc &o) noexcept {
    if (this != &o) {
      Arc tmp(o);
      swap(tmp);
    }
    return *this;
  }

  Arc &operator=(Arc &&o) noexcept {
    if (this != &o) {
      Arc tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~Arc() noexcept {
    if (m_inner) _::arc_dec_strong(m_inner);
  }

  Arc clone() const noexcept {
    return Arc(*this);
  }

  /**
   * @brief Strong count (debugging / coarse instrumentation).
   *
   * Loads with relaxed — by the time you read it another thread may
   * have changed it, so don't branch on the result.
   */
  size_t strong_count() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return m_inner->strong.load(std::memory_order_relaxed);
  }

  /**
   * @brief Weak count (debugging only). Subtracts the +1 for "all
   *        strongs", matching Rust's Arc::weak_count.
   */
  size_t weak_count() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    const size_t w = m_inner->weak.load(std::memory_order_relaxed);
    return w - 1;
  }

  T &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return m_inner->value;
  }
  T *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return &m_inner->value;
  }
  T *get() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return &m_inner->value;
  }

  void swap(Arc &o) noexcept {
    _::ArcInner<T, Alloc> *tmp = m_inner;
    m_inner                    = o.m_inner;
    o.m_inner                  = tmp;
  }

  bool operator==(const Arc &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const Arc &o) const noexcept {
    return m_inner != o.m_inner;
  }

  static Arc clone(const Arc &r) noexcept {
    return r.clone();
  }

  static ArcWeak<T, Alloc> downgrade(const Arc &r) noexcept;

  /* ── internals exposed only to friends / templates ───────────────── */

  _::ArcInner<T, Alloc> *inner_raw() const noexcept {
    return m_inner;
  }
  void inner_raw_reset() noexcept {
    m_inner = nullptr;
  }

private:
  explicit Arc(_::ArcInner<T, Alloc> *inner) noexcept : m_inner(inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
  }

  _::ArcInner<T, Alloc> *m_inner;

  template <class, class> friend class Arc;
  template <class, class> friend class ArcWeak;
  friend class Option<Arc<T, Alloc>>;

public:
  /* ── factories ──────────────────────────────────────────────────── */

  /**
   * @brief Construct an Arc<T, Alloc> in place using a default-constructed
   *        Alloc.
   *
   * Single heap allocation containing the strong count (=1), the weak
   * count (=1, the "all strongs as one weak" marker), and T constructed
   * from @p args.
   *
   * If the first argument is convertible to `Alloc`, it is treated as
   * the allocator instance (for stateful allocators); otherwise, the
   * Alloc is default-constructed and all arguments are forwarded to T's
   * constructor. For the ambiguous case (T's first ctor arg is
   * convertible to Alloc), use `make_in` explicitly.
   */
  template <class... Args,
            class = typename std::enable_if<!_::FirstIsAlloc<Alloc, Args...>::value>::type>
  static Arc make(Args &&...args) {
    return make_in(Alloc{}, std::forward<Args>(args)...);
  }

  /** @brief Overload that accepts an Alloc instance as the first argument. */
  template <class First, class... Rest,
            class = typename std::enable_if<
              std::is_convertible<typename std::decay<First>::type, Alloc>::value>::type>
  static Arc make(First &&alloc, Rest &&...rest) {
    return make_in(std::forward<First>(alloc), std::forward<Rest>(rest)...);
  }

  /**
   * @brief Construct an Arc<T, Alloc> with an explicit allocator instance.
   *
   * Use this when `make`'s SFINAE detection is ambiguous (T's first
   * ctor arg is convertible to Alloc) or when you want to be explicit
   * about which argument is the allocator.
   */
  template <class AllocArg, class... Args> static Arc make_in(AllocArg &&alloc, Args &&...args) {
    Layout layout = Layout::of<_::ArcInner<T, Alloc>>();
    auto   r      = alloc.allocate(layout);
    XPP_ASSERT(r.is_ok(), "Arc::make_in: allocation failed");
    void                  *mem = r.unwrap().data();
    _::ArcInner<T, Alloc> *inner =
      ::new (mem) _::ArcInner<T, Alloc>(std::forward<AllocArg>(alloc), std::forward<Args>(args)...);
    return Arc(inner);
  }
};

template <class T, class Alloc> void swap(Arc<T, Alloc> &a, Arc<T, Alloc> &b) noexcept {
  a.swap(b);
}

/**
 * @brief Niche-optimized Option<Arc<T, Alloc>>. Storage is a single
 *        ArcInner<T, Alloc>* whose nullptr value means None.
 *
 * Works for any Alloc because sizeof(Arc<T, Alloc>) == sizeof(T*)
 * (the Alloc lives in ArcInner, not in the Arc handle).
 */
template <class T, class Alloc> class Option<Arc<T, Alloc>> {
public:
  using value_type = Arc<T, Alloc>;

  constexpr Option() noexcept : m_inner(nullptr) {}
  constexpr Option(None) noexcept : m_inner(nullptr) {}

  Option(const Arc<T, Alloc> &r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    m_inner->strong.fetch_add(1, std::memory_order_relaxed);
  }
  Option(Arc<T, Alloc> &&r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    r.m_inner = nullptr;
  }

  Option(const Option &o) noexcept : m_inner(o.m_inner) {
    if (m_inner) m_inner->strong.fetch_add(1, std::memory_order_relaxed);
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
    if (m_inner) _::arc_dec_strong(m_inner);
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

  Arc<T, Alloc> unwrap() && {
    XPP_ASSERT(m_inner != nullptr, "unwrap() on None Option");
    _::ArcInner<T, Alloc> *taken = m_inner;
    m_inner                      = nullptr;
    return Arc<T, Alloc>(taken);
  }

  Arc<T, Alloc> unwrap_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Option must be Some");
    _::ArcInner<T, Alloc> *taken = m_inner;
    m_inner                      = nullptr;
    return Arc<T, Alloc>(taken);
  }

  Arc<T, Alloc> take() {
    return std::move(*this).unwrap();
  }

  void swap(Option &o) noexcept {
    _::ArcInner<T, Alloc> *tmp = m_inner;
    m_inner                    = o.m_inner;
    o.m_inner                  = tmp;
  }

private:
  _::ArcInner<T, Alloc> *m_inner;

  friend class ArcWeak<T, Alloc>; // for ArcWeak::upgrade to build a Some directly
};

template <class T, class Alloc>
void swap(Option<Arc<T, Alloc>> &a, Option<Arc<T, Alloc>> &b) noexcept {
  a.swap(b);
}

/**
 * @brief Non-owning, thread-safe observer of an Arc<T, Alloc>'s inner.
 *
 * Default-constructs to null. Constructing from an Arc bumps the
 * inner's weak count atomically. `upgrade()` does a CAS loop on the
 * strong count so it sees a consistent "is anyone still strong?"
 * snapshot even if another thread is dropping the last Arc.
 *
 * Does not store its own Alloc instance — the Alloc lives in ArcInner,
 * which is shared between Arc and ArcWeak. The Alloc type parameter
 * is only used so `arc_dec_weak_and_maybe_dealloc` knows which
 * instantiation to call.
 */
template <class T, class Alloc> class ArcWeak {
public:
  constexpr ArcWeak() noexcept : m_inner(nullptr) {}

  explicit ArcWeak(const Arc<T, Alloc> &r) noexcept : m_inner(r.inner_raw()) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    m_inner->weak.fetch_add(1, std::memory_order_relaxed);
  }

  ArcWeak(const ArcWeak &o) noexcept : m_inner(o.m_inner) {
    if (m_inner) m_inner->weak.fetch_add(1, std::memory_order_relaxed);
  }
  ArcWeak(ArcWeak &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  ArcWeak &operator=(const ArcWeak &o) noexcept {
    if (this != &o) {
      ArcWeak tmp(o);
      swap(tmp);
    }
    return *this;
  }
  ArcWeak &operator=(ArcWeak &&o) noexcept {
    if (this != &o) {
      ArcWeak tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~ArcWeak() noexcept {
    if (m_inner) _::arc_dec_weak_and_maybe_dealloc(m_inner);
  }

  /**
   * @brief Attempt to obtain a strong Arc.
   *
   * CAS loop: read strong, fail if 0, else compare-exchange to
   * strong+1. The compare-exchange is `acquire` on success so the
   * resulting Arc synchronises-with prior strong drops (we need to
   * see all the writes that landed before the last drop, in case
   * another thread happens to be in the middle of dropping); the
   * failure load can be relaxed.
   *
   * Returns None if every strong has been dropped (T destroyed) or
   * this ArcWeak is null.
   */
  Option<Arc<T, Alloc>> upgrade() const noexcept {
    if (!m_inner) return Option<Arc<T, Alloc>>();
    size_t s = m_inner->strong.load(std::memory_order_relaxed);
    for (;;) {
      if (s == 0) return Option<Arc<T, Alloc>>();
      if (m_inner->strong.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
        Option<Arc<T, Alloc>> out;
        out.m_inner = m_inner;
        return out;
      }
      // s was updated to the current value by the failed exchange;
      // loop and retry.
    }
  }

  size_t strong_count() const noexcept {
    return m_inner ? m_inner->strong.load(std::memory_order_relaxed) : 0;
  }

  size_t weak_count() const noexcept {
    if (!m_inner) return 0;
    const size_t s = m_inner->strong.load(std::memory_order_relaxed);
    const size_t w = m_inner->weak.load(std::memory_order_relaxed);
    return s > 0 ? w - 1 : w;
  }

  bool is_expired() const noexcept {
    return !m_inner || m_inner->strong.load(std::memory_order_relaxed) == 0;
  }

  void swap(ArcWeak &o) noexcept {
    _::ArcInner<T, Alloc> *tmp = m_inner;
    m_inner                    = o.m_inner;
    o.m_inner                  = tmp;
  }

  bool operator==(const ArcWeak &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const ArcWeak &o) const noexcept {
    return m_inner != o.m_inner;
  }

private:
  _::ArcInner<T, Alloc> *m_inner;
};

template <class T, class Alloc> void swap(ArcWeak<T, Alloc> &a, ArcWeak<T, Alloc> &b) noexcept {
  a.swap(b);
}

/* ── deferred member impl: Arc<T, Alloc>::downgrade ─────────────────── */

template <class T, class Alloc>
inline ArcWeak<T, Alloc> Arc<T, Alloc>::downgrade(const Arc<T, Alloc> &r) noexcept {
  return ArcWeak<T, Alloc>(r);
}

/* ── invariants pinned at compile time ───────────────────────────────── */

static_assert(sizeof(Arc<int>) == sizeof(int *), "Arc<T> must be sizeof(T*)");
static_assert(sizeof(Option<Arc<int>>) == sizeof(int *),
              "Option<Arc<T>> niche optimisation broken");
static_assert(sizeof(ArcWeak<int>) == sizeof(int *), "ArcWeak<T> must be sizeof(T*)");

} // namespace xpp

#endif // XPP_ARC_H
