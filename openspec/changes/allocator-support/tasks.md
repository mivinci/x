## 1. Core allocator types (allocator.h)

- [ ] 1.1 Define `AllocError` (empty struct)
- [ ] 1.2 Define `Layout` struct `{ size_t size; size_t align; }` with `Layout::of<T>()` and `Layout::array(n, align)`
- [ ] 1.3 Define `AllocInfo` struct `{ void* ptr; size_t size; }`
- [ ] 1.4 Define `GlobalAllocator` — `allocate` (C++17 aligned new, fallback non-aligned), `deallocate`. Empty class.
- [ ] 1.5 Define `default_grow<A>(A&, void*, Layout, Layout)` and `default_shrink<A>(A&, void*, Layout, Layout)` free functions (allocate + memcpy + deallocate)
- [ ] 1.6 Define SFINAE helpers: `has_grow<A>`, `has_shrink<A>` — detect custom grow/shrink methods
- [ ] 1.7 Define `allocator_grow<A>(A&, ...)` / `allocator_shrink<A>(A&, ...)` — dispatches to custom or default

## 2. Migrate Arc

- [ ] 2.1 Add `Alloc` template parameter to `Arc<T, Alloc = GlobalAllocator>`
- [ ] 2.2 Add `Alloc` to `ArcInner<T, Alloc>`, store as member (EBO if empty)
- [ ] 2.3 Update `ArcInner` constructor to accept `Alloc` instance
- [ ] 2.4 Update `arc_dec_strong` and `arc_dec_weak_and_maybe_dealloc` to move alloc out, destruct inner, then dealloc
- [ ] 2.5 Update `Arc::make()` — detect if first arg is Alloc (SFINAE), otherwise default-construct
- [ ] 2.6 Update `ArcWeak<T, Alloc>` — same Alloc parameter, no alloc storage (shares via ArcInner)
- [ ] 2.7 Update `Option<Arc<T, Alloc>>` niche specialization — use SFINAE to only niche-optimize when `Alloc` is empty

## 3. Migrate Rc

- [ ] 3.1 Add `Alloc` template parameter to `Rc<T, Alloc = GlobalAllocator>` (same pattern as Arc)
- [ ] 3.2 Update `RcInner<T, Alloc>`, `rc_dec_strong`, `rc_dec_weak_and_maybe_dealloc`
- [ ] 3.3 Update `Rc::make()` — same SFINAE pattern as Arc
- [ ] 3.4 Update `Weak<T, Alloc>` — same Alloc parameter
- [ ] 3.5 Update `Option<Rc<T, Alloc>>` niche specialization

## 4. Migrate Own (Deleter → Alloc)

- [ ] 4.1 Replace `Deleter` template parameter with `Alloc = GlobalAllocator` on `Own<T, Alloc>`
- [ ] 4.2 Replace `Box<T, Deleter>` with `Box<T, Alloc>` (same change)
- [ ] 4.3 Update `CompressedPair<T*, Alloc>` — same EBO logic, just renamed
- [ ] 4.4 Update `Own` destructor: `ptr->~T()` then `alloc.deallocate(ptr, Layout::of<T>())`
- [ ] 4.5 Update `Own(T*, Alloc)` constructor (replaces `Own(T*, Deleter)`)
- [ ] 4.6 Update `Box` destructor: same pattern
- [ ] 4.7 Update covariant constructors (`Own<Derived, E>` → `Own<Base, A>`) to require `Alloc` compatibility
- [ ] 4.8 Update `Own::take()` / `release()` — return raw `T*` (caller manages, no Alloc)

## 5. Migrate NonNull

- [ ] 5.1 `NonNull<T>` currently has no Deleter — no change needed (non-owning)
- [ ] 5.2 Verify `NonNull` works with `Alloc`-parameterized `Own`/`Box` (covariant, etc.)

## 6. Update Promise (Arc usage)

- [ ] 6.1 `ResolveState` uses `Arc<ResolveState<T>>` — update to `Arc<ResolveState<T>, GlobalAllocator>` (fixed, don't expose Alloc)
- [ ] 6.2 Verify all `Arc::make()` calls in `promise_adapter.h` still compile with default Alloc

## 7. Tests

- [ ] 7.1 `GlobalAllocator` — allocate/deallocate basic types
- [ ] 7.2 `Arc<T, GlobalAllocator>` — same behavior as before (regression)
- [ ] 7.3 `Arc<T, CountingAlloc>` — stateful allocator, verify alloc/dealloc counts
- [ ] 7.4 `Own<T, GlobalAllocator>` — same behavior as before (regression)
- [ ] 7.5 `Own<T, CountingAlloc>` — stateful allocator, verify counts
- [ ] 7.6 `Layout::of<T>()` — correct size/align for various types
- [ ] 7.7 `grow`/`shrink` — default implementation correctness
- [ ] 7.8 `grow`/`shrink` — custom implementation (CountingAlloc with realloc)
- [ ] 7.9 EBO verification: `sizeof(Arc<T, GlobalAllocator>) == sizeof(T*)`
- [ ] 7.10 EBO verification: `sizeof(Own<T, GlobalAllocator>) == sizeof(T*)`
- [ ] 7.11 Stateful alloc: `sizeof(Arc<T, StatefulAlloc>) == sizeof(T*)` (alloc in ArcInner, not in Arc)
- [ ] 7.12 `Option<Arc<T, GlobalAllocator>>` — niche optimization still works (sizeof == sizeof(T*))
- [ ] 7.13 Cross-thread Arc with stateful alloc — alloc/dealloc on correct thread

## 8. Docs

- [ ] 8.1 Create `docs/libxpp/allocator.md` — Allocator protocol, GlobalAllocator, custom allocator examples
- [ ] 8.2 Update `docs/libxpp/smart-pointers/README.md` — add Alloc parameter to table
- [ ] 8.3 Update `docs/libxpp/smart-pointers/arc.md` — Alloc parameter, make() with allocator
- [ ] 8.4 Update `docs/libxpp/smart-pointers/own.md` — Deleter → Alloc migration
- [ ] 8.5 Update `docs/SUMMARY.md` — add allocator page
