## Why

The smart pointer family (`Arc`, `Rc`, `Own`, `Box`) currently hardcodes `::operator new` / `::operator delete`. Users who need custom allocation (arena, slab, pool) have no way to plug in their own allocator. This is especially important for embedded systems, game engines, and high-performance servers where allocation control is critical.

## What Changes

- Define `AllocError`, `Layout`, `AllocInfo` types in a new `allocator.h` header.
- Define the Allocator protocol: `allocate(Layout) → Result<AllocInfo, AllocError>`, `deallocate(void*, Layout)`, `grow(void*, old, new)`, `shrink(void*, old, new)`.
- Define `GlobalAllocator` — default allocator wrapping `::operator new` / `::operator delete` (C++17 aligned variants when available). Empty class → EBO → 0 bytes overhead.
- Add `Alloc` template parameter to `Arc<T, Alloc>`, `ArcWeak<T, Alloc>`, `Rc<T, Alloc>`, `Weak<T, Alloc>`, `Own<T, Alloc>`, `Box<T, Alloc>`. Default: `GlobalAllocator`.
- For `Arc`/`Rc`: allocator stored in `ArcInner`/`RcInner` (EBO if empty). `sizeof(Arc<T, Alloc>) == sizeof(T*)` always.
- For `Own`/`Box`: allocator stored via `CompressedPair` (already supports EBO). `sizeof(Own<T, Alloc>) == sizeof(T*)` when `Alloc` is empty.
- Replace `Deleter` parameter on `Own<T>` / `Box<T>` with `Alloc` parameter. Old `Deleter` interface (`operator()(T*)`) replaced by `Alloc::deallocate(void*, Layout)`.
- `make()` methods accept optional allocator argument for stateful allocators.
- Default `grow`/`shrink` implementations (allocate + copy + deallocate) provided as free functions. Allocators can override for optimization (e.g. `realloc`).
- Update `Option<Arc<T>>` / `Option<Rc<T>>` niche specializations to handle `Alloc` parameter.

## Capabilities

### New Capabilities
- `allocator`: Core allocator types (`AllocError`, `Layout`, `AllocInfo`) and the Allocator protocol (`allocate`/`deallocate`/`grow`/`shrink`). Includes `GlobalAllocator` default implementation.

### Modified Capabilities
- `arc`: `Arc<T, Alloc>` and `ArcWeak<T, Alloc>` gain `Alloc` template parameter. Allocator stored in `ArcInner`. `make()` accepts optional allocator.
- `rc`: `Rc<T, Alloc>` and `Weak<T, Alloc>` gain `Alloc` template parameter. Allocator stored in `RcInner`. `make()` accepts optional allocator.
- `own`: `Own<T, Alloc>` replaces `Deleter` with `Alloc`. `CompressedPair` already supports EBO. `Own(T*, Alloc)` constructor replaces `Own(T*, Deleter)`.
- `box`: `Box<T, Alloc>` replaces `Deleter` with `Alloc`. Same `CompressedPair` EBO.

## Impact

- **New files**: `libxpp/xpp/allocator.h`, `libxpp/xpp/allocator_test.cpp`
- **Modified files**: `arc.h`, `rc.h`, `weak.h`, `own.h`, `box.h`, `nonnull.h` (Deleter → Alloc), `CMakeLists.txt`
- **Breaking**: `Own<T, Deleter>` → `Own<T, Alloc>`. Custom deleters must be rewritten as allocators. `std::default_delete<T>` → `GlobalAllocator`.
- **Non-goals**: PromiseNode arena allocation (separate change), `std::allocator` compatibility, `pmr` integration, `Option<Arc<T, Alloc>>` niche optimization for non-empty Alloc (degrade to member storage).
