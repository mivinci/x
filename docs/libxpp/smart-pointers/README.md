# Smart Pointers

[← libxpp](../README.md)

Rust-inspired smart pointers with `sizeof == sizeof(T*)` guarantees. All are header-only, C++11-compatible.

## Overview

| Type | Ownership | Thread-safe | Header |
|------|-----------|-------------|--------|
| [`Own<T>`](own.md) | Unique, nullable | No | `own.h` |
| [`Box<T>`](box.md) | Unique, non-null | No | `box.h` |
| [`Rc<T>`](rc.md) | Shared | No | `rc.h` |
| [`Weak<T>`](rc.md) | Weak observer for `Rc` | No | `weak.h` |
| [`Arc<T>`](arc.md) | Shared | Yes (atomic) | `arc.h` |
| [`ArcWeak<T>`](arc.md) | Weak observer for `Arc` | Yes (atomic) | `arc.h` |
| [`NonNull<T>`](nonnull.md) | Non-owning, non-null | No | `nonnull.h` |

## Key Design Choices

- **Single pointer storage**: `sizeof == sizeof(T*)` for all types. No two-word `shared_ptr` layout.
- **Niche-optimized `Option`**: `Option<Arc<T>>` and `Option<Rc<T>>` are also `sizeof(T*)` — `nullptr = None`.
- **Non-intrusive**: `RcInner<T> = { strong, weak, value }` in a single heap allocation. T doesn't inherit anything.
- **Rust-style refcount**: weak count includes +1 for "all strongs as one weak". `weak_count()` subtracts this to match Rust semantics.
- **No custom deleter, no aliasing constructor.** Use `make()` — no construction from raw `T*`.
- **Arc memory orders**: `relaxed` for clone, `release` for drop, `acquire` fence only when count hits 0. Matches Rust libstd / triomphe / boost.

## Covariant Up-cast

`Rc<Derived>` → `Rc<Base>` and `Arc<Derived>` → `Arc<Base>` work via covariant constructors (copy and move).

## Comparison with std

| Feature | xpp | std |
|---------|-----|-----|
| `sizeof` | `sizeof(T*)` | `2 * sizeof(T*)` |
| Control block | Co-located (single alloc) | Separate alloc or intrusive |
| Custom deleter | No | Yes |
| `make()` | Single alloc | Single alloc (but 2-block fallback) |
| `Option<T>` niche | Yes (nullptr = None) | No |
| Thread-safe variant | `Arc` (separate type) | `shared_ptr` (same type, always atomic) |
