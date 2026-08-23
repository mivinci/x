# Smart Pointers

[← libxpp](../README.md)

Rust-inspired smart pointers with `sizeof == sizeof(T*)` guarantees. All are header-only, C++11-compatible.

## Overview

| Type | Ownership | Thread-safe | Header |
|------|-----------|-------------|--------|
| [`Own<T, Allocator>`](own.md) | Unique, nullable | No | `own.h` |
| [`Box<T, Allocator>`](box.md) | Unique, non-null | No | `box.h` |
| [`Rc<T, Allocator>`](rc.md) | Shared | No | `rc.h` |
| [`Weak<T, Allocator>`](rc.md) | Weak observer for `Rc` | No | `weak.h` |
| [`Arc<T, Allocator>`](arc.md) | Shared | Yes (atomic) | `arc.h` |
| [`ArcWeak<T, Allocator>`](arc.md) | Weak observer for `Arc` | Yes (atomic) | `arc.h` |
| [`NonNull<T>`](nonnull.md) | Non-owning, non-null | No | `nonnull.h` |

The `XPP_MT` / `shared.h` abstraction was removed: the library's internals
(`Bytes`, channels, promise state) always use `Arc<T>` (atomic refcount).
`Rc<T>` remains for user code that wants explicit zero-atomic-overhead
single-threaded sharing.

All owning types default to [`GlobalAllocator`](../allocator.md) and
accept a custom `Allocator` template parameter. Empty allocators (like
`GlobalAllocator`) incur zero storage overhead via EBO.

## Key Design Choices

- **Single pointer storage**: `sizeof == sizeof(T*)` for all types. No two-word `shared_ptr` layout.
- **Niche-optimized `Option`**: `Option<Arc<T>>` and `Option<Rc<T>>` are also `sizeof(T*)` — `nullptr = None`.
- **Non-intrusive**: `RcInner<T, Allocator> = { strong, weak, value, alloc }` in a single heap allocation. T doesn't inherit anything.
- **Rust-style refcount**: weak count includes +1 for "all strongs as one weak". `weak_count()` subtracts this to match Rust semantics.
- **Allocator protocol**: `Allocator` parameter (default `GlobalAllocator`) controls allocation/deallocation. Stored in control block (Arc/Rc) or via `CompressedPair` (Own/Box) with EBO. See [Allocator](../allocator.md).
- **Arc memory orders**: `relaxed` for clone, `release` for drop, `acquire` fence only when count hits 0. Matches Rust libstd / triomphe / boost.

## Covariant Up-cast

`Rc<Derived, Allocator>` → `Rc<Base, Allocator>` and `Arc<Derived, Allocator>` → `Arc<Base, Allocator>` work via covariant constructors (copy and move). Same `Allocator` required.

## What xpp Has That STL Doesn't

### Niche-Optimized `Option<T>`

The single biggest practical win. In xpp, `Option<Own<T>>`, `Option<Box<T>>`, `Option<Rc<T>>`, `Option<Arc<T>>`, and `Option<NonNull<T>>` all have `sizeof == sizeof(T*)`. The `None` state is encoded via the null pointer — a value that normal construction never produces.

```cpp
// STL: 16 bytes (8-byte pointer + bool + alignment padding)
std::optional<std::unique_ptr<int>> parent;

// xpp: 8 bytes — same size as a raw pointer
Option<Own<Node>> parent;
```

In tree, graph, or AST data structures where every node has an `Option<parent>` / `Option<child>`, this saves 8+ bytes per field. A million-node tree saves ~8 MB just on the parent edge alone.

**Why STL can't do this:** `std::optional` must be generic over all types, and `std::unique_ptr(nullptr)` is a valid (non-empty) state. xpp's smart pointers have a constructor-level invariant that null is unreachable — the type system guarantees a stored null pointer means `None`.

### Non-Null by Default — `Box<T>`

STL has no equivalent. `std::unique_ptr` default-constructs to null, forcing null checks at every use site. `Box<T>` has **no default constructor** — if you have one, it owns a valid object. The compiler enforces this.

```cpp
// STL: always defensive
void process(std::unique_ptr<Widget> w) {
  if (!w) return;          // ← who knows what the caller passed
  w->do_thing();
}

// xpp: type system gives the guarantee
void process(Box<Widget> w) {
  w->do_thing();           // ← never null, compiler-checked
}
```

Combined with `Option<Box<T>>`, you get explicit opt-in nullability at zero space cost — exactly Rust's model.

### Single-Threaded `Rc<T>` (No Atomic Overhead)

`std::shared_ptr`'s control block is **always** atomic, even when you know you're single-threaded. Every copy and destroy pays the memory barrier. xpp splits this into two types:

- `Rc<T>` — plain `int` refcount, zero atomic overhead, for event-loop or single-thread code
- `Arc<T>` — atomic refcount, for cross-thread sharing

In the dominant xpp use case (single-thread event loops), `Rc<T>` avoids all `shared_ptr`'s atomic penalties.

### Semantic Layering — Pick the Right Tool

| Need | STL gives you | xpp gives you |
|---|---|---|
| Maybe-null, unique ownership | `unique_ptr<T>` | `Own<T>` |
| **Never-null**, unique ownership | — | `Box<T>` |
| Maybe-null, shared ownership | `shared_ptr<T>` | `Rc<T>` or `Arc<T>` |
| Maybe-null, non-owning observer | `weak_ptr<T>` | `Weak<T>` or `ArcWeak<T>` |
| Never-null, non-owning pointer | raw `T*` | `NonNull<T>` |

`Box<T>` and `NonNull<T>` have no STL counterpart — they encode non-null guarantees in the type system that raw pointers and `unique_ptr` leave to convention.

### Promise Ecosystem Integration

xpp smart pointers compose directly with `Promise<T>` chains — no glue code:

```cpp
Promise<Own<Data>> fetch() {
  return Promise<void>::after(100).then([]() {
    return Own<Data>(new Data{42});  // Own flows through then()
  });
}

// Own → Box: take ownership, guarantee non-null downstream
auto boxed = fetch().await()
  .into_nonnull()   // Option<Box<Data>>
  .unwrap();        // Box<Data>
```

### Single-Pointer Layout for All Types

All xpp smart pointers are `sizeof(T*)`. `Rc<T>` and `Arc<T>` point directly to a co-located `RcInner { strong, weak, T }` block — single allocation, single pointer. `std::shared_ptr` is two pointers (object + control block), doubling stack/struct footprint.

---

## Comparison with std

| Feature | xpp | std |
|---------|-----|-----|
| `sizeof` (unique) | `sizeof(T*)` | `sizeof(T*)` |
| `sizeof` (shared) | `sizeof(T*)` | `2 × sizeof(T*)` |
| Non-null default | `Box<T>` | — |
| Niche Option | Yes (`nullptr = None`) | No |
| Single-thread shared | `Rc<T>` (no atomics) | `shared_ptr` (always atomic) |
| Thread-safe shared | `Arc<T>` | `shared_ptr` |
| Custom allocator | Yes (`Allocator` template param, compile-time) | `std::pmr` (type-erased, runtime) |
| Allocator storage | Control block (Arc/Rc) or `CompressedPair` (Own/Box), EBO when empty | vtable ptr in control block (always) |
| Deallocation | `~T()` + `alloc.deallocate()` (separated) | `deleter(ptr)` (single call) |
| Covariant upcast | Implicit (same `Allocator`) | Implicit |
| Weak observer | `Weak<T>` / `ArcWeak<T>` | `weak_ptr<T>` |
| Promise interop | Native (`.then()`, `into_nonnull()`) | N/A |
| Control block | Co-located (single alloc) | Separate or intrusive |
| Header-only | Yes | Yes |
