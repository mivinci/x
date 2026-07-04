# Allocator

libx++ smart pointers (`Arc`, `Rc`, `Own`, `Box`) accept an optional
`Alloc` template parameter that controls how the control block (or the
pointed-to object) is allocated and deallocated. The default
`GlobalAllocator` uses `::operator new` / `::operator delete` and is
empty (zero overhead via EBO).

The protocol is modeled after Rust's `std::alloc::Allocator` trait.

## Protocol

An Allocator is a class type with two required methods and two optional
ones:

```cpp
struct MyAlloc {
  // Required: allocate at least layout.size bytes with layout.align alignment.
  // Returns the allocated span (pointer + actual size, >= layout.size)
  // or an AllocError.
  xpp::Result<xpp::Span<uint8_t>, xpp::AllocError> allocate(xpp::Layout layout) const;

  // Required: free memory previously returned by allocate.
  // layout must match the Layout passed to allocate.
  void deallocate(void *ptr, xpp::Layout layout) const noexcept;

  // Optional: grow/shrink an existing allocation in place if possible.
  // If not provided, default_grow / default_shrink (allocate + memcpy +
  // deallocate) are used.
  xpp::Result<xpp::Span<uint8_t>, xpp::AllocError> grow(void *ptr, xpp::Layout old_l,
                                                          xpp::Layout new_l) const;
  xpp::Result<xpp::Span<uint8_t>, xpp::AllocError> shrink(void *ptr, xpp::Layout old_l,
                                                            xpp::Layout new_l) const;
};
```

`allocate` and `deallocate` are `const`-qualified (Rust `&self` style).
Stateful allocators track state via `mutable` members or atomic
pointers — `std::atomic<T>::fetch_add` is itself `const`, so counters
held by pointer work without any `mutable` dance.

### Layout

`Layout` bundles `size` and `align` so they're always passed together:

```cpp
struct Layout {
  size_t size;
  size_t align;

  template <class T> static Layout of();        // sizeof(T), alignof(T)
  static Layout array(size_t n, size_t a);      // n bytes, a alignment
};
```

### AllocError

`AllocError` is an empty struct. `allocate` returns
`Result<Span<uint8_t>, AllocError>` — callers must handle the error
path. (Smart pointers panic on allocation failure, matching the
existing `::operator new` behavior that throws `std::bad_alloc`.)

### Span<uint8_t>

`allocate` returns a `Span<uint8_t>` — a non-owning fat pointer
(pointer + length). The length is the **actual** allocated size, which
may be larger than the requested `layout.size` (allocate-at-least
semantics). Smart pointers ignore the length; it's there for callers
that want to use the slack.

## GlobalAllocator

The default. Empty class → EBO-eligible → zero storage overhead in
`ArcInner` / `CompressedPair`.

```cpp
struct GlobalAllocator {
  Result<Span<uint8_t>, AllocError> allocate(Layout layout) const;
  void deallocate(void *ptr, Layout layout) const;
};
```

Uses C++17 aligned `::operator new` / `::operator delete` when
available; falls back to non-aligned on C++11.

## Using a custom allocator

### With Arc / Rc

`Arc<T, Alloc>` stores the `Alloc` instance inside `ArcInner<T, Alloc>`
(with EBO when `Alloc` is empty). `sizeof(Arc<T, A>) == sizeof(T*)`
for **any** `A` — the allocator lives in the control block, not in the
handle.

```cpp
// Stateless alloc (default-constructed):
auto a = Arc<T, MyArenaAlloc>::make(args...);

// Stateful alloc (passed in):
auto a = Arc<T, ArenaAlloc>::make(my_alloc, args...);

// Explicit (when SFINAE detection is ambiguous):
auto a = Arc<T, ArenaAlloc>::make_in(my_alloc, args...);
```

`make()` uses SFINAE to detect whether the first argument is an
`Alloc` instance (for stateful allocators) or a constructor argument
for `T`. `make_in()` is the explicit form that always treats the first
argument as the allocator.

### With Own / Box

`Own<T, Alloc>` and `Box<T, Alloc>` store the `Alloc` via
`CompressedPair<T*, Alloc>` with EBO. `sizeof(Own<T>) == sizeof(T*)`
when `Alloc` is empty; grows by `sizeof(Alloc)` when stateful.

```cpp
// Default GlobalAllocator:
Own<int> o(new int(42));

// Stateful alloc:
Own<int, MyAlloc> o(new int(42), MyAlloc{...});

// Access the allocator:
o.get_allocator();

// Box is the non-null variant:
Box<int, MyAlloc> b = Box<int, MyAlloc>::from_raw(new int(42), MyAlloc{...});
```

The destructor calls `ptr->~T()` explicitly, then
`alloc.deallocate(ptr, Layout::of<T>())` — separating object destruction
from memory deallocation.

## Custom allocator example

```cpp
struct CountingAllocator {
  std::atomic<int> *allocs;
  std::atomic<int> *deallocs;

  CountingAllocator(std::atomic<int> *a, std::atomic<int> *d)
      : allocs(a), deallocs(d) {}

  xpp::Result<xpp::Span<uint8_t>, xpp::AllocError> allocate(xpp::Layout layout) const {
    void *p = ::operator new(layout.size);
    if (!p) return xpp::Result<xpp::Span<uint8_t>, xpp::AllocError>(xpp::err, xpp::AllocError{});
    allocs->fetch_add(1, std::memory_order_relaxed);
    return xpp::Result<xpp::Span<uint8_t>, xpp::AllocError>(
        xpp::ok, xpp::Span<uint8_t>(static_cast<uint8_t *>(p), layout.size));
  }

  void deallocate(void *ptr, xpp::Layout) const {
    deallocs->fetch_add(1, std::memory_order_relaxed);
    ::operator delete(ptr);
  }
};

std::atomic<int> allocs{0}, deallocs{0};
CountingAllocator alloc(&allocs, &deallocs);

{
  auto a = xpp::Arc<int, CountingAllocator>::make(alloc, 42);
  EXPECT_EQ(allocs.load(), 1);
}
EXPECT_EQ(deallocs.load(), 1);
```

## grow / shrink

`grow` and `shrink` are optional. If your allocator doesn't provide
them, use the free functions `default_grow` / `default_shrink`, which
implement them as allocate + memcpy + deallocate:

```cpp
auto r = xpp::default_grow(alloc, old_ptr, old_layout, new_layout);
```

To provide a custom `grow`/`shrink` (e.g. in-place `realloc`), add the
methods to your allocator and call them directly:

```cpp
struct ReallocAllocator {
  // ... allocate / deallocate ...

  xpp::Result<xpp::Span<uint8_t>, xpp::AllocError> grow(void *ptr, xpp::Layout old_l,
                                                          xpp::Layout new_l) const {
    // Try realloc first; fall back to allocate+copy+dealloc.
    void *p = ::realloc(ptr, new_l.size);
    if (!p) return xpp::default_grow(*this, ptr, old_l, new_l);
    return xpp::Result<xpp::Span<uint8_t>, xpp::AllocError>(
        xpp::ok, xpp::Span<uint8_t>(static_cast<uint8_t *>(p), new_l.size));
  }
};
```

## EBO (Empty Base Optimization)

When `Alloc` is an empty class (like `GlobalAllocator`), it is stored
as a **base class** of the control block (`ArcInner` / `RcInner`) or
via `CompressedPair` (`Own` / `Box`), not as a member. This gives
zero storage overhead:

- `sizeof(Arc<T, GlobalAllocator>) == sizeof(T*)`
- `sizeof(Own<T, GlobalAllocator>) == sizeof(T*)`
- `sizeof(ArcInner<T, GlobalAllocator>) == sizeof(strong) + sizeof(weak) + sizeof(T)` (no Alloc byte)

When `Alloc` is stateful, it is stored as a member and `sizeof` grows
by `sizeof(Alloc)` (rounded for alignment). The `Arc`/`Rc`/`ArcWeak`/
`Weak` handles themselves stay at `sizeof(T*)` because the `Alloc`
lives in the control block, not in the handle — only `Own`/`Box` grow
because they have no separate control block.

## Deallocation lifecycle (Arc/Rc)

The trickiest part of the stateful-allocator path: the `Alloc` lives
**inside** the control block that it's about to free. The deallocator
moves the `Alloc` out before freeing:

```cpp
// In arc_dec_weak_and_maybe_dealloc:
if (inner->weak.fetch_sub(1, std::memory_order_release) == 1) {
  std::atomic_thread_fence(std::memory_order_acquire);
  Alloc a = std::move(inner->alloc_ref());   // move out
  Layout layout = Layout::of<ArcInner<T, Alloc>>();
  a.deallocate(inner, layout);                // free the memory that contained `a`
}
```

`Alloc` must be move-constructible (enforced by `static_assert` in
`Arc`/`Rc`). For empty allocators the move is trivial; for stateful
allocators it should be `noexcept` (move the resource handle, not copy
it).
