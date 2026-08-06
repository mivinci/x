# Vec<T, Allocator> — Design Proposal

## Summary

A contiguous growable array type — xpp's equivalent of Rust `std::vec::Vec`, built
on xpp's existing allocator protocol. Replaces `std::vector<T>` for all xpp-owned
code paths.

## Motivation

`std::vector<T>` is inadequate for xpp for four reasons:

1. **Exception-based OOM.** `push_back` throws `std::bad_alloc`. xpp is
   no-exceptions. `try_push` must return `Result<bool, AllocError>`.
2. **Allocator is a fixed type parameter.** `std::vector<T, ArenaAlloc>` and
   `std::vector<T, GlobalAlloc>` are different types — can't unify in a
   container or pass to a function expecting "any Vec<T>".
3. **No `try_` API surface.** No `try_reserve`, no `try_resize`. OOM handling
   requires catching exceptions, which xpp forbids.
4. **API mismatch with Rust patterns.** xpp's `String` needs `split_off` to
   implement `split_at`, `extend_from_slice` for `push_str`. `std::vector`
   has `insert`/`erase` (position-based) instead of `split_off`/`truncate`
   (length-based).

## Relationship to Existing xpp Types

```
Own<T, A>   → single object, allocator-parameterised
Box<T, A>   → single object, guaranteed non-null, allocator-parameterised
Vec<T, A>   → multiple objects, contiguous memory, allocator-parameterised
Span<T>     → non-owning view, zero-overhead
```

Vec completes the trio: `Own`/`Box` handle single allocation; Vec handles
dynamic arrays.

## Type-vs-Allocator Separation (Key Design Choice)

Unlike `std::vector<T, Alloc>`, xpp Vec stores the allocator as a *runtime
field* via `CompressedPair`, not as a fixed template parameter that changes
the type identity.

```cpp
// These are the SAME type:
Vec<int, GlobalAllocator>  v1;
Vec<int, ArenaAlloc>       v2;

// Can be placed in the same container:
std::vector<Vec<int>> vecs;
vecs.push_back(Vec<int>(global_alloc));
vecs.push_back(Vec<int>(arena_alloc));
```

This is achieved by type-erasing the allocator through the existing
`allocate(Layout)` / `deallocate(ptr, Layout)` protocol. Vec's growth
strategy is type-independent — it operates on raw bytes via `Layout`.

**Tradeoff:** the `Alloc` template parameter still exists for EBO (empty
`GlobalAllocator` contributes zero size). But two Vecs with different
allocators are the same type as far as the type system is concerned.

## API Reference

```cpp
namespace xpp {

template <class T, class Alloc = GlobalAllocator>
class Vec {
public:
    using value_type = T;
    using allocator_type = Alloc;

    /* ── Construction ── */

    Vec() = default;
    explicit Vec(Alloc alloc);
    explicit Vec(size_t capacity, Alloc alloc = Alloc{});
    Vec(const Vec& other) requires std::is_copy_constructible<T>::value;
    Vec(Vec&& other) noexcept;
    ~Vec();

    Vec& operator=(const Vec&) requires std::is_copy_constructible<T>::value;
    Vec& operator=(Vec&&) noexcept;

    /* ── Borrowing ── */

    /** Non-owning view. O(1). */
    Span<T> as_span();
    Span<const T> as_span() const;

    /** Raw pointer access. O(1). */
    const T* data() const noexcept;
    T* data() noexcept;

    /* ── Capacity ── */

    /** Number of elements. O(1). */
    size_t len() const noexcept;

    /** Allocated capacity. O(1). */
    size_t capacity() const noexcept;

    bool empty() const noexcept;

    /** Reserve at least `additional` more elements.
     *  XPP_ASSERT on OOM — use try_reserve() for explicit error handling. */
    void reserve(size_t additional);

    /** Reserve at least `additional` more elements.
     *  Returns AllocError on OOM, or Ok on success. */
    Result<void, AllocError> try_reserve(size_t additional);

    /** Shrink capacity to fit len(). O(n) if realloc.
     *  XPP_ASSERT on OOM — use try_shrink_to_fit() for explicit handling. */
    void shrink_to_fit();

    /** Shrink capacity to fit len(). O(n) if realloc. */
    Result<void, AllocError> try_shrink_to_fit();

    /* ── Element Access ── */

    /** Unchecked access. XPP_ASSERT in debug. O(1). */
    T& operator[](size_t index);
    const T& operator[](size_t index) const;

    /** Checked access. Returns None if index >= len(). O(1). */
    Option<T&> get(size_t index);
    Option<const T&> get(size_t index) const;

    /** Returns the first element, or None if empty. O(1). Same as get(0). */
    Option<T&> first();
    Option<const T&> first() const;

    /** Returns the last element, or None if empty. O(1). Same as get(len()-1). */
    Option<T&> last();
    Option<const T&> last() const;

    /* ── Mutation ── */

    /** Append an element. O(1) amortised.
     *  XPP_ASSERT on OOM — use try_push() for explicit error handling. */
    void push(const T& value);
    void push(T&& value);

    /** Append an element. Returns AllocError on OOM. O(1) amortised. */
    Result<void, AllocError> try_push(const T& value);
    Result<void, AllocError> try_push(T&& value);

    /** Push an element that fits within capacity without reallocating.
     *  XPP_ASSERT(len() < capacity()) — caller must have reserved first.
     *  O(1). Use in hot inner loops after a reserve() call. */
    void push_unchecked(T&& value);

    /** Remove the last element. Returns None if empty. O(1). */
    Option<T> pop();

    /** Remove all elements (calls destructors, does NOT free capacity). O(n). */
    void clear();

    /** Truncate to `new_len`. No-op if new_len >= len(). O(1). */
    void truncate(size_t new_len);

    /** Resize to `new_len`, filling new slots with copies of `fill`.
     *  Drops excess elements if shrinking. O(n) if growing.
     *  XPP_ASSERT on OOM — use try_resize() for explicit error handling. */
    void resize(size_t new_len, const T& fill);

    /** Resize to `new_len`, filling new slots with copies of `fill`.
     *  Drops excess elements if shrinking. O(n) if growing. */
    Result<void, AllocError> try_resize(size_t new_len, const T& fill);

    /** Move all elements from `other` into `this`. `other` becomes empty.
     *  May reallocate. O(n + other.len()).
     *  XPP_ASSERT on OOM — use try_append() for explicit error handling. */
    void append(Vec& other);

    /** Move all elements from `other` into `this`. `other` becomes empty.
     *  May reallocate. O(n + other.len()). */
    Result<void, AllocError> try_append(Vec& other);

    /* ── Splitting ── */

    /** Split off the tail starting at `at`. `at` must be <= len().
     *  `this` retains [0, at); returned Vec has [at, len()).
     *  O(n) — copies elements. */
    Vec split_off(size_t at);

    /* ── Element Removal ── */

    /** Remove the element at `index`, replacing it with the last element.
     *  Does NOT preserve order. O(1). XPP_ASSERT index < len(). */
    T swap_remove(size_t index);

    /** Retain elements for which `pred(x)` returns true. O(n). */
    template <class Pred>
    void retain(Pred pred);

    /* ── Iteration ── */

    T* begin() noexcept;
    T* end() noexcept;
    const T* begin() const noexcept;
    const T* end() const noexcept;

    /* ── Allocator Access ── */

    Alloc& allocator() noexcept;
    const Alloc& allocator() const noexcept;

private:
    T*      m_ptr = nullptr;
    size_t  m_len = 0;
    size_t  m_cap = 0;
    Alloc   m_alloc;
};

} // namespace xpp
```

### Growth Strategy

```cpp
// Default growth: double, minimum 4
size_t grow_cap(size_t old_cap, size_t required) {
    size_t new_cap = old_cap > 0 ? old_cap * 2 : 4;
    while (new_cap < required) new_cap *= 2;
    return new_cap;
}
```

Uses `default_grow` / `default_shrink` from `allocator.h` when the allocator
doesn't provide its own `grow`/`shrink`. This means `Vec` transparently
benefits from arena bump-allocation or any custom growth strategy.

### Size and Layout

```
sizeof(Vec<T>)     == sizeof(T*) + sizeof(size_t) × 2 + sizeof(Alloc)
                       = 8 + 8 + 8 + 0 = 24 (GlobalAllocator, 64-bit, EBO)
sizeof(Span<T>)    == 16 (ptr + len, same as always)
```

No hidden allocations beyond the heap buffer. EBO eliminates allocator overhead
for the default `GlobalAllocator`.

## Rust Vec Method Coverage — Full Analysis

Each Rust `Vec` method is mapped to the xpp equivalent, with implementation
notes for non-trivial cases.

### ✅ L0 — Implement now (Phase 1–3)

#### Construction & Decomposition

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `Vec::new()` | `Vec()` | Default constructor. `m_ptr=nullptr, m_len=0, m_cap=0`. |
| `Vec::with_capacity(n)` | `Vec(size_t cap, Alloc a={})` | `allocate(Layout::array(n, alignof(T)))`. Fails means Vec is empty — constructor can't return Result, so use a two-phase init: `Vec v; v.try_reserve(n)`. Or make the capacity-constructor abort on OOM (debug assertion). |

> **`into_raw_parts()`/`from_raw_parts()` are intentionally omitted.**
> `data()` + `len()` + `capacity()` already expose the raw parts as accessors.
> The consuming/reconstructing variants add risk (UB on mismatched allocator)
> without enabling anything that can't be done with the existing accessors.

#### Borrowing & Views

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `as_slice() → &[T]` | `as_span() → Span<const T>` | `Span(m_ptr, m_len)`. Zero-cost view. |
| `as_mut_slice() → &mut [T]` | `as_span() → Span<T>` (non-const overload) | Same, non-const. |
| `as_ptr() → *const T` | `data() → const T*` | Direct pointer access. Valid until next mutation. |
| `as_mut_ptr() → *mut T` | `data() → T*` (non-const overload) | Same, non-const. |

#### Capacity

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `len() → usize` | `len() → size_t` | |
| `is_empty() → bool` | `empty() → bool` | |
| `capacity() → usize` | `capacity() → size_t` | |
| `reserve(n)` | `reserve(n)` + `try_reserve(n) → Result` | `reserve` uses `try_reserve(...).expect("OOM")`. Growth: `max(old*2, required, 4)` via `default_grow`. |
| `reserve_exact(n)` | `try_reserve_exact(n) → Result` | Phase 2 — minimal allocation, no amortised headroom. Rarely needed; add when demanded. |
| `try_reserve(n)` | Same as `try_reserve` — xpp has no separate panicking path | xpp doesn't distinguish; `try_reserve` *is* the only reserve. |
| `try_reserve_exact(n)` | Same as `try_reserve_exact` | |
| `shrink_to_fit()` | `shrink_to_fit()` + `try_shrink_to_fit() → Result` | `shrink_to_fit` uses `try_shrink_to_fit(...).expect("OOM")`. Reallocate to `m_len`. |
| `shrink_to(n)` | Defer to L0.1 | Rarely needed. Callers can `try_reserve` + `try_shrink_to_fit`. |

#### Element Access

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `operator[] → &T` (Index) | `operator[](size_t) → T&` | `XPP_ASSERT(index < m_len)`. No bounds check in release. |
| `operator[] → &mut T` (IndexMut) | Same via non-const overload | |
| `get(i) → Option<&T>` | `get(i) → Option<T&>` | Bounds-checked. `i >= m_len → None`. |
| `get_mut(i) → Option<&mut T>` | `get(i) → Option<T&>` (non-const) | Same. |
| `get_many_mut([i,j])` | **Not planned** | Requires compile-time index-distinctness checking. C++ can't express this safely. |
| `first() → Option<&T>` | `first() → Option<T&>` | Bounds-checked. Returns None if empty. Same as `get(0)`. |
| `first_mut() → Option<&mut T>` | `first() → Option<T&>` (non-const) | Same. |
| `last() → Option<&T>` | `last() → Option<T&>` | Bounds-checked. Returns None if empty. Same as `get(len()-1)`. |
| `last_mut() → Option<&mut T>` | `last() → Option<T&>` (non-const) | Same. |

#### Mutation — Growth

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `push(value)` | `push(const T&)` + `push(T&&)` | Two overloads (lvalue/rvalue). Convenience wrappers call `try_push` with `XPP_ASSERT(is_ok())`. |
| `push_within_capacity(value)` | `push_unchecked(value)` | `XPP_ASSERT(len() < cap())`. Hot-loop optimisation after `reserve()`. |
| `try_push(value)` | `try_push(value)` — same name | |
| `try_push_ref(&value)` | `try_push(const T&) → Result` | Copy-construct, not move. |
| `pop() → Option<T>` | `pop() → Option<T>` | Move out last element, decrement `m_len`. `m_len==0 → None`. |
| `append(&mut other)` | `append(Vec&)` + `try_append(Vec&) → Result` | `append` uses `try_append(...).expect("OOM")`. O(other.len()). |
| `extend_from_slice(&[T])` | `try_push` loop | Not a named method — `for (auto& x : slice) v.try_push(x)` is explicit. O(n) realloc risk is visible. |
| `extend_from_within(range)` | **Not planned** | Self-referential extension. Rare and easily misused. |
| `resize(n, value)` | `resize(n, fill)` + `try_resize(n, fill) → Result` | `resize` uses `try_resize(...).expect("OOM")`. |
| `resize_with(n, f)` | Defer to L0.1 | Needs closure ergonomics. Can do `if (v.len() < n) { v.try_reserve(n - v.len()); while (v.len() < n) v.try_push(f()); }` manually. |
| `try_resize(n, value)` | Same as `try_resize` — xpp has no panicking version | |

#### Mutation — Shrink / Remove

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `clear()` | `clear()` | Call `~T()` on each element (reverse order), set `m_len=0`. Does NOT free capacity. |
| `truncate(n)` | `truncate(n)` | If `n < m_len`: destroy elements `[n, m_len)`, set `m_len=n`. O(1) if n >= m_len. |
| `drain(range)` | Defer to L0.1 | Returns an iterator that yields removed elements. C++11 needs iterator-pair API carefully designed to avoid dangling. |
| `retain(f)` | `retain(pred)` | Two-pointer scan: read ptr skips rejected elements; write ptr compact-saves kept elements. Destroy tail. O(n). |
| `retain_mut(f)` | `retain(pred)` (pred takes `T&`) | Same algorithm, mutable predicate. |
| `dedup()` | Defer to L0.1 | Needs `operator==` on T. Two-pointer compact: `if (*read != *write) swap; write++`. |
| `dedup_by(f)` | Defer to L0.1 | Same with custom comparator `bool(*)(const T&, const T&)`. |
| `dedup_by_key(f)` | Defer to L0.1 | Same with key extractor `K(*)(const T&)`. Rarely needed. |
| `remove(index)` | Defer to L0.1 | O(n) by design (all subsequent elements shift left). Use `swap_remove` for O(1) unordered removal. |
| `swap_remove(index)` | `swap_remove(index)` | Replace `self[index]` with `self.last()`, then pop(). O(1). Does NOT preserve order. |
| `extract_if(pred)` | Defer to L0.1 | Like `drain` but with predicate. C++11 lambdas less ergonomic. |

#### Splitting

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `split_off(at)` | `split_off(at) → Vec` | Create new Vec with capacity `m_len - at`, move elements `[at, m_len)` into it. `this` retains `[0, at)`. O(n) copy. |
| `split_at(n) → (&[T], &[T])` | Defer to L0.1 | Returns two Span views. Non-owning, trivial — just two `Span(m_ptr, n)` and `Span(m_ptr+n, m_len-n)`. |
| `split_at_mut(n)` | Defer to L0.1 | Same, mutable. |

#### Reordering

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `reverse()` | Defer to L0.1 | `std::swap(m_ptr[i], m_ptr[len-1-i])` loop. 10 lines. |
| `swap(i, j)` | Defer to L0.1 | `std::swap(m_ptr[i], m_ptr[j])`. 3 lines. |
| `sort()` / `sort_by()` ... | **Not planned** | Use `std::sort(v.begin(), v.end())` directly. No wrapper needed. |
| `select_nth_unstable()` | **Not planned** | `<algorithm>` provides `std::nth_element`. |

#### Search

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `binary_search(&T)` | **Not planned** | Use `std::binary_search(v.begin(), v.end(), val)`. |
| `binary_search_by(f)` | **Not planned** | Use `std::binary_search` with custom comparator. |
| `contains(&T)` | Defer to L0.1 | Trivial: `std::find(begin(), end(), val) != end()`. 3 lines. |

#### Iteration

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `iter() → Iter<T>` | `begin() → const T*` / `end() → const T*` | Raw pointers are valid C++ iterators. Range-for works: `for (auto& x : v)`. |
| `iter_mut() → IterMut<T>` | `begin() → T*` / `end() → T*` (non-const) | Same, mutable. |
| `IntoIterator for Vec` | Same — move iterates over owned elements | `for (auto& x : std::move(v))` — but ownership semantics differ from Rust. Use `pop()` loop for consuming iteration. |
| `IntoIterator for &Vec` | `begin()` const overload | |
| `IntoIterator for &mut Vec` | `begin()` non-const overload | |

#### Conversion

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `into_boxed_slice() → Box<[T]>` | **Not planned** | xpp has no `Box<[T]>` type. |
| `leak() → &'static mut [T]` | **Not planned** | Intentionally leaking memory violates RAII principles. |

#### Unsafe / Low-level

| Rust | xpp | Implementation notes |
|------|-----|---------------------|
| `set_len(n)` | Defer to L0.1 | Extremely dangerous — sets `m_len` without initialising elements. Only for `MaybeUninit`-style manual init. Mark `XPP_UNSAFE`. |
| `spare_capacity_mut()` | Defer to L0.1 | Returns slice of uninitialised capacity. For manual init patterns. |
| `split_at_spare_mut()` | **Not planned** | Very niche — manual init with splitting. |

---

### Summary

| Category | Count | Decision |
|----------|-------|----------|
| ✅ L0 — Phase 1–3 | **29** methods | Core construction, access, push/pop, reserve/resize, append, split_off, swap_remove, retain, iterators. Each grow-path has dual API: convenience (`push`, `reserve`...) using `.expect()`, and explicit (`try_push`, `try_reserve`...) returning `Result`. |
| ⚠️ L0.1 — Deferred | **16** methods | insert/remove/drain, dedup, reverse, swap, contains, set_len, etc. |
| ❌ Not planned | **7** methods | leak, into_boxed_slice, sort/binary_search (use `<algorithm>`), get_many_mut, extend_from_within, select/splice |

### ⚠️ L0.1 — Deferred (detailed rationale)

| Rust | Why deferred | When to add |
|------|-------------|------------|
| `reserve_exact(n)` | Rarely needed. `reserve` + `shrink_to_fit` combo works. | First concrete use case |
| `shrink_to(n)` | Same. | |
| `resize_with(n, f)` | Closure ergonomics. Callers can loop manually. | When it becomes a common pattern |
| `insert(index, v)` / `remove(index)` | O(n) by design. Name makes cost invisible. | If natural API wins over explicit cost |
| `drain(range)` / `extract_if(pred)` | Iterator API design non-trivial in C++11 | If batch-remove patterns emerge |
| `dedup()` / `dedup_by()` | Needs `operator==` or comparator. | When duplicate-removal is common |
| `reverse()` | Trivial algorithm, callers can write inline. | When used in 3+ places |
| `swap(i, j)` | Same — `std::swap` is one line. | |
| `contains(&T)` | `std::find` one-liner. | When used in 3+ places |
| `split_at(n)` | Trivial — two `Span` views. | When used in 3+ places |
| `set_len(n)` | Extremely dangerous. Needs `XPP_UNSAFE` annotation. | Only if manual initialisation is required |
| `spare_capacity_mut()` | Niche — manual init patterns. | Same as above |

## What's NOT Provided

- `insert(index)` / `remove(index)` — O(n) shift. Use `swap_remove` for O(1)
  unordered removal, or `split_off` + `try_append` for ordered.
- Bounds-checked `operator[]` — use `get(index)` instead. `operator[]` is
  debug-assertion only, matching Rust's `Index` trait behaviour.
- Iterator invalidation guarantees beyond "don't mutate during iteration".
  This is C++, not Rust — the compiler won't save you.

## File Placement

```
libxpp/xpp/vec.h    — Vec<T, Alloc> (~350 lines)
libxpp/xpp/vec_test.cpp
```

Dependencies: `xpp/allocator.h`, `xpp/option.h`, `xpp/result.h`, `xpp/span.h`,
`xpp/panic.h`, `xpp/compressed_pair.h`.

## Implementation Plan

**Phase 1: Core (∼200 lines)**
- Construction (default, capacity, copy, move)
- Destructor (destroy elements, deallocate)
- `data()`, `len()`, `capacity()`, `empty()`
- `as_span()`
- `operator[]`, `get()`, `first()`, `last()`
- `try_push`, `pop`, `clear`, `truncate`
- Growth logic (`default_grow` integration)
- Tests: push/pop/basic access/capacity growth

**Phase 2: Mutation (∼100 lines)**
- `try_reserve`, `try_shrink_to_fit`
- `try_resize`, `try_append`
- `swap_remove`, `retain`
- `split_off`
- Tests: reserve/resize/split/append/swap_remove

**Phase 3: Advanced (∼30 lines)**
- Iterator pair: `begin()` / `end()`
- Tests: range-for compatibility

## C++11 Compatibility

- No `noexcept(auto)` or conditional `noexcept` — use explicit `noexcept`
  on move constructor / destructor paths
- No `requires` clauses on copy — use SFINAE via `enable_if` on templated
  copy constructor instead
- No `if constexpr` — use tag dispatch or SFINAE
- No `consteval` — no compile-time checks needed for Vec (it's a runtime type)

## Open Questions

1. **`try_reserve_exact`?** Rust has both `reserve` (may allocate more) and
   `reserve_exact` (exact). xpp just has `try_reserve` with the growth
   strategy documented. Callers wanting exact can call `try_shrink_to_fit`
   afterwards. Keep it simple.

2. **`Vec::make(alloc, args...)` factory?** Like `Box::make` and `Own::make`,
   this would construct T in-place. Worth adding in Phase 2 — useful for
   non-copyable types.

3. **`into_std_vector()`?** Could be useful for interop. `reinterpret_cast`
   has the same aliasing problem as `String::into_std_string`, so it'd be
   an O(n) copy. Defer until there's a concrete need.
