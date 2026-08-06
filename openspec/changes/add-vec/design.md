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

template <typename T, typename Alloc = GlobalAllocator>
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
     *  Returns AllocError on OOM, or Ok on success. */
    Result<void, AllocError> try_reserve(size_t additional);

    /** Shrink capacity to fit len(). O(n) if realloc. */
    Result<void, AllocError> try_shrink_to_fit();

    /* ── Element Access ── */

    /** Unchecked access. XPP_ASSERT in debug. O(1). */
    T& operator[](size_t index);
    const T& operator[](size_t index) const;

    /** Checked access. Returns None if index >= len(). O(1). */
    Option<T&> get(size_t index);
    Option<const T&> get(size_t index) const;

    T& first();
    const T& first() const;
    T& last();
    const T& last() const;

    /* ── Mutation ── */

    /** Append an element. Returns AllocError on OOM. O(1) amortised. */
    Result<void, AllocError> try_push(T value);
    Result<void, AllocError> try_push(const T& value);

    /** Remove the last element. Returns None if empty. O(1). */
    Option<T> pop();

    /** Remove all elements (calls destructors, does NOT free capacity). O(n). */
    void clear();

    /** Truncate to `new_len`. No-op if new_len >= len(). O(1). */
    void truncate(size_t new_len);

    /** Resize to `new_len`, filling new slots with copies of `fill`.
     *  Drops excess elements if shrinking. O(n) if growing. */
    Result<void, AllocError> try_resize(size_t new_len, const T& fill);

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

    /* ── Raw Parts (Advanced) ── */

    /** Decompose into raw pointer + length + capacity. O(1).
     *  The caller owns the memory and must eventually call
     *  Vec::from_raw_parts() or manually deallocate. */
    std::tuple<T*, size_t, size_t> into_raw_parts() &&;

    /** Reconstruct from raw pointer + length + capacity. O(1).
     *  Caller guarantees the pointer came from into_raw_parts() of a
     *  Vec with the same allocator type. */
    static Vec from_raw_parts(T* ptr, size_t len, size_t cap, Alloc alloc = Alloc{});

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

## Rust Vec Method Coverage

### ✅ L0 — Implement now

| Rust | xpp | Notes |
|------|-----|-------|
| `Vec::new()` | `Vec()` | Default constructor |
| `Vec::with_capacity(n)` | `Vec(size_t capacity)` | |
| `Vec::len()` | `len()` | |
| `Vec::capacity()` | `capacity()` | |
| `Vec::is_empty()` | `empty()` | |
| `Vec::as_slice()` / `as_mut_slice()` | `as_span()` | |
| `Vec::as_ptr()` / `as_mut_ptr()` | `data()` | |
| `Vec::get(index)` | `get(index) → Option<T&>` | |
| `Vec::first()` / `last()` | `first()` / `last()` | |
| `Vec::push(value)` | `try_push(value) → Result` | Returns AllocError on OOM |
| `Vec::pop()` | `pop() → Option<T>` | |
| `Vec::clear()` | `clear()` | |
| `Vec::truncate(n)` | `truncate(n)` | |
| `Vec::resize(n, fill)` | `try_resize(n, fill) → Result` | |
| `Vec::reserve(n)` | `try_reserve(n) → Result` | |
| `Vec::shrink_to_fit()` | `try_shrink_to_fit() → Result` | |
| `Vec::append(&mut other)` | `try_append(Vec&) → Result` | |
| `Vec::split_off(at)` | `split_off(at) → Vec` | |
| `Vec::swap_remove(index)` | `swap_remove(index)` | |
| `Vec::retain(pred)` | `retain(pred)` | |
| `Vec::into_raw_parts()` | `into_raw_parts() → (ptr, len, cap)` | |
| `Vec::from_raw_parts(...)` | `from_raw_parts(ptr, len, cap)` | |
| `Index` / `IndexMut` | `operator[]` | No bounds check (debug assertion) |
| `IntoIterator` / `Iter` / `IterMut` | `begin()` / `end()` | Raw pointers as iterators |
| `Vec::extend_from_slice()` | `try_push` loop | Not a named method — O(n) realloc risk is explicit |

**L0 total: 25 methods**

### ⚠️ L0.1 — Deferred (nice to have, not blocking)

| Rust | Why deferred |
|------|-------------|
| `Vec::insert(index, value)` | O(n) shift — hide the cost |
| `Vec::remove(index)` | Same as above |
| `Vec::drain(range)` | Needs C++11 iterator-pair API design |
| `Vec::splice(range, other)` | Complex — low demand |
| `Vec::dedup()` / `dedup_by()` | Needs `PartialEq` trait or comparator |
| `Vec::resize_with(n, fn)` | Needs `FnOnce`/closure ergonomics |
| `Vec::extract_if(pred)` | C++11 lambda capture limits |
| `Vec::sort()` / `binary_search()` | Use `<algorithm>` directly on `data()` |

### ❌ Not planned

| Rust | Reason |
|------|--------|
| `Vec::leak()` | Intentionally leaking is anti-RAII in C++ |
| `Vec::into_boxed_slice()` | xpp has no `Box<[T]>` equivalent |
| `Vec::try_collect()` | Requires Rust-style `FromIterator` trait |

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

**Phase 3: Advanced (∼50 lines)**
- `into_raw_parts` / `from_raw_parts`
- Iterator pair: `begin()` / `end()`
- Tests: raw parts round-trip, range-for compatibility

## C++11 Compatibility

- No `noexcept(auto)` or conditional `noexcept` — use explicit `noexcept`
  on move constructor / destructor paths
- No `requires` clauses on copy — use SFINAE via `enable_if` on templated
  copy constructor instead
- No `std::tuple` for `into_raw_parts` return — use `struct RawParts { T* ptr; size_t len; size_t cap; };`
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
