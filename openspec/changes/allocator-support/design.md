## Context

The smart pointers use `::operator new` / `::operator delete` directly. `Own<T>` and `Box<T>` use a `Deleter` template parameter (function object with `operator()(T*)`), while `Arc<T>` and `Rc<T>` have no allocation customization at all.

The goal is to unify on an Allocator interface (modeled after Rust's `std::alloc::Allocator` trait) that handles both allocation and deallocation, with `grow`/`shrink` for completeness.

## Goals / Non-Goals

**Goals:**
- Allocator protocol with `allocate`, `deallocate`, `grow`, `shrink`
- `Result<AllocInfo, AllocError>` return type for `allocate`/`grow`/`shrink` (type-safe error handling)
- `Layout` struct bundling `size + align`
- `GlobalAllocator` default (empty class → EBO → 0 bytes)
- `Arc<T, Alloc>`, `Rc<T, Alloc>`, `Own<T, Alloc>`, `Box<T, Alloc>` with `Alloc` parameter
- Allocator stored in control block (`ArcInner`/`RcInner`) or via `CompressedPair` (`Own`/`Box`)
- `sizeof` unchanged for empty allocators (EBO)
- `make()` accepts optional allocator for stateful allocators

**Non-Goals:**
- PromiseNode arena allocation (separate change)
- `std::allocator` / STL allocator concept compatibility
- `std::pmr::memory_resource` integration
- `Option<Arc<T, Alloc>>` niche optimization for non-empty `Alloc` (fall back to `bool + Arc` storage)
- `std::allocate_at_least` semantics (return actual allocated size — simplified, `AllocInfo.size == Layout.size` for default)

## Decisions

### D1: Allocator stored in control block (Arc/Rc), not in pointer

```
Arc<T, Alloc> → pointer to ArcInner<T, Alloc> {
  Alloc alloc;              ← stored here, EBO if empty
  atomic<size_t> strong;
  atomic<size_t> weak;
  T value;
}
sizeof(Arc<T, Alloc>) == sizeof(T*)  ← always
```

Rationale: same as Rust's `Arc<T, A>`. Allocator available at deallocation time (last weak drop). EBO ensures zero overhead for empty allocators.

### D2: Allocator stored in CompressedPair (Own/Box), replacing Deleter

```
Own<T, Alloc> → CompressedPair<T*, Alloc>
  empty Alloc → EBO → sizeof == sizeof(T*)
  stateful Alloc → sizeof == sizeof(T*) + sizeof(Alloc)
```

`CompressedPair` already exists and handles EBO. No new infrastructure needed.

### D3: Deleter → Alloc migration

Old: `Own<T, Deleter>` where `Deleter::operator()(T* ptr)` calls `delete ptr`.
New: `Own<T, Alloc>` where `Alloc::deallocate(void* ptr, Layout)` calls `::operator delete(ptr)`.

Destruction order changes:
- Old: `deleter(ptr)` → `delete ptr` → `~T()` + `operator delete`
- New: `ptr->~T()` (explicit) → `alloc.deallocate(ptr, Layout::of<T>())`

The explicit `~T()` call is necessary because `deallocate` only frees memory, not objects.

### D4: Layout struct

```cpp
struct Layout {
  size_t size;
  size_t align;

  template <class T>
  static Layout of() { return { sizeof(T), alignof(T) }; }
};
```

Bundles size + align so they're always passed together (same as Rust's `Layout`). Avoids bugs from mismatched size/align pairs.

### D5: Result<AllocInfo, AllocError> return type

```cpp
struct AllocError {};

struct AllocInfo {
  void*  ptr;   // non-null on success
  size_t size;  // actual allocated size (>= layout.size)
};
```

`allocate` returns `Result<AllocInfo, AllocError>`. Forces callers to handle failure. `AllocInfo.ptr` is guaranteed non-null on success.

### D6: grow/shrink as optional methods with default implementations

Allocators MUST provide `allocate` and `deallocate`. `grow`/`shrink` are OPTIONAL — if not provided, default free functions (`default_grow`, `default_shrink`) implement them as allocate + memcpy + deallocate.

SFINAE detects whether the Allocator type has custom `grow`/`shrink`:
```cpp
// If A has grow(): use it
// Else: use default_grow(a, ...)
```

### D7: GlobalAllocator

```cpp
struct GlobalAllocator {
  Result<AllocInfo, AllocError> allocate(Layout layout);
  void deallocate(void* ptr, Layout layout);
  // grow/shrink: not provided → use defaults
};
```

Empty class → EBO → `sizeof(GlobalAllocator) == 0` (via CompressedPair or ArcInner member). C++17 uses aligned `operator new`/`delete`; C++11 falls back to non-aligned.

### D8: make() API

```cpp
// Stateless Alloc (default):
auto a = Arc<T>::make(args...);                    // uses GlobalAllocator{}

// Stateless Alloc (explicit):
auto a = Arc<T, MyArenaAlloc>::make(args...);     // uses MyArenaAlloc{}

// Stateful Alloc:
auto a = Arc<T, ArenaAlloc>::make(arena, args...); // uses arena
```

`make()` detects whether first argument is an Alloc instance or a constructor argument for T. Implemented via SFINAE: if first arg is convertible to `Alloc`, treat as allocator; otherwise, default-construct `Alloc`.

## Risks / Trade-offs

- **[Breaking change]** `Own<T, Deleter>` → `Own<T, Alloc>`. All custom deleters must be rewritten. `std::default_delete<T>` → `GlobalAllocator`.
- **[Option<Arc<T, Alloc>> niche]** For non-empty `Alloc`, niche optimization (nullptr = None) doesn't work cleanly. Fall back to `Option` storing `Arc<T, Alloc>` as a member (sizeof increases). Only affects non-default allocators — rare.
- **[grow/shrink SFINAE complexity]** Detecting custom `grow`/`shrink` requires `void_t` SFINAE. C++11 doesn't have `void_t` — need to define it.
- **[Deallocation order]** Must explicitly call `~T()` before `deallocate`. Current `Deleter` does both in `delete`. Forgetting `~T()` = leak (memory freed, destructor not called).
- **[Alloc copy in dealloc]** For Arc/Rc, allocator must be moved out of `ArcInner` before `deallocate` (since `deallocate` frees `ArcInner` itself). Move must be noexcept — `Alloc` should have noexcept move.
