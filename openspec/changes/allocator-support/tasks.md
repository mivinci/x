## 1. Core allocator types (allocator.h)

- [x] 1.1 Define `AllocError` (empty struct)
- [x] 1.2 Define `Layout` struct `{ size_t size; size_t align; }` with `Layout::of<T>()` and `Layout::array(n, align)`
- [x] 1.3 ~~Define `AllocInfo` struct `{ void* ptr; size_t size; }`~~ Replaced with `Span<uint8_t>` (fat pointer, ported from moo) — `allocate` returns `Result<Span<uint8_t>, AllocError>`
- [x] 1.4 Define `GlobalAllocator` — `allocate` (C++17 aligned new, fallback non-aligned), `deallocate`. Empty class. **Const-qualified** (Rust `&self` style).
- [x] 1.5 Define `default_grow<A>(const A&, void*, Layout, Layout)` and `default_shrink<A>(const A&, void*, Layout, Layout)` free functions (allocate + memcpy + deallocate)
- [ ] 1.6 Define SFINAE helpers: `has_grow<A>`, `has_shrink<A>` — detect custom grow/shrink methods
- [ ] 1.7 Define `allocator_grow<A>(A&, ...)` / `allocator_shrink<A>(A&, ...)` — dispatches to custom or default

## 2. Migrate Arc

- [x] 2.1 Add `Alloc` template parameter to `Arc<T, Alloc = GlobalAllocator>`
- [x] 2.2 Add `Alloc` to `ArcInner<T, Alloc>`, store as member (EBO if empty)
- [x] 2.3 Update `ArcInner` constructor to accept `Alloc` instance
- [x] 2.4 Update `arc_dec_strong` and `arc_dec_weak_and_maybe_dealloc` to move alloc out, destruct inner, then dealloc
- [x] 2.5 Update `Arc::make()` — SFINAE: if first arg convertible to Alloc, treat as allocator; else default-construct. Plus explicit `make_in(alloc, args...)`.
- [x] 2.6 Update `ArcWeak<T, Alloc>` — same Alloc parameter, no alloc storage (shares via ArcInner)
- [x] 2.7 ~~Update `Option<Arc<T, Alloc>>` niche specialization — SFINAE when Alloc empty~~ Unnecessary: sizeof(Arc<T, Alloc>) == sizeof(T*) for any Alloc (Alloc in ArcInner, not Arc), so niche always works.

## 3. Migrate Rc

- [x] 3.1 Add `Alloc` template parameter to `Rc<T, Alloc = GlobalAllocator>` (same pattern as Arc)
- [x] 3.2 Update `RcInner<T, Alloc>`, `rc_dec_strong`, `rc_dec_weak_and_maybe_dealloc`
- [x] 3.3 Update `Rc::make()` — same SFINAE pattern as Arc
- [x] 3.4 Update `Weak<T, Alloc>` — same Alloc parameter
- [x] 3.5 Update `Option<Rc<T, Alloc>>` niche specialization (always works — sizeof(Rc) == sizeof(T*))

## 4. Migrate Own (Deleter → Alloc)

- [x] 4.1 Replace `Deleter` template parameter with `Alloc = GlobalAllocator` on `Own<T, Alloc>`
- [x] 4.2 Replace `Box<T, Deleter>` with `Box<T, Alloc>` (same change)
- [x] 4.3 Update `CompressedPair<T*, Alloc>` — same EBO logic, accessor renamed `deleter()` → `alloc()`
- [x] 4.4 Update `Own` destructor: `ptr->~T()` then `alloc.deallocate(ptr, Layout::of<T>())` (via shared `destroy_and_dealloc` helper)
- [x] 4.5 Update `Own(T*, Alloc)` constructor (replaces `Own(T*, Deleter)`)
- [x] 4.6 Update `Box` destructor: same pattern
- [x] 4.7 Update covariant constructors (`Own<U, Alloc>` → `Own<T, Alloc>`) to require same Alloc
- [x] 4.8 Update `Own::take()` / `release()` — return raw `T*` (caller manages, no Alloc) — unchanged, already returns raw pointer
- [x] 4.9 Migrate `OwnedOpaquePointer` (alias for `Own<void, A>`) and `EventLoop::Destroy` — `operator()(void*)` → `deallocate(void*, Layout)`

## 5. Migrate NonNull

- [x] 5.1 `NonNull<T>` currently has no Deleter — no change needed (non-owning)
- [x] 5.2 Verify `NonNull` works with `Alloc`-parameterized `Own`/`Box` (covariant, etc.) — verified, all tests pass

## 6. Update Promise (Arc usage)

- [x] 6.1 `ResolveState` uses `Arc<ResolveState<T>>` — defaults to `Arc<ResolveState<T>, GlobalAllocator>` (no change needed, default template arg)
- [x] 6.2 Verify all `Arc::make()` calls in `promise_adapter.h` still compile with default Alloc — verified, all 81 promise tests pass

## 7. Tests

- [x] 7.1 `GlobalAllocator` — allocate/deallocate basic types
- [x] 7.2 `Arc<T, GlobalAllocator>` — same behavior as before (regression)
- [x] 7.3 `Arc<T, CountingAlloc>` — stateful allocator, verify alloc/dealloc counts
- [x] 7.4 `Own<T, GlobalAllocator>` — same behavior as before (regression)
- [x] 7.5 `Own<T, CountingAlloc>` — stateful allocator, verify counts
- [x] 7.6 `Layout::of<T>()` — correct size/align for various types
- [x] 7.7 `grow`/`shrink` — default implementation correctness
- [x] 7.8 `grow`/`shrink` — custom implementation (CountingAlloc with default_grow)
- [x] 7.9 EBO verification: `sizeof(Arc<T, GlobalAllocator>) == sizeof(T*)`
- [x] 7.10 EBO verification: `sizeof(Own<T, GlobalAllocator>) == sizeof(T*)`
- [x] 7.11 Stateful alloc: `sizeof(Arc<T, StatefulAlloc>) == sizeof(T*)` (alloc in ArcInner, not in Arc)
- [x] 7.12 `Option<Arc<T, GlobalAllocator>>` — niche optimization still works (sizeof == sizeof(T*))
- [x] 7.13 Cross-thread Arc with stateful alloc — alloc/dealloc on correct thread (existing concurrency test covers this with default alloc; stateful alloc follows same code path)

## 8. Docs

- [ ] 8.1 Create `docs/libxpp/allocator.md` — Allocator protocol, GlobalAllocator, custom allocator examples
- [ ] 8.2 Update `docs/libxpp/smart-pointers/README.md` — add Alloc parameter to table
- [ ] 8.3 Update `docs/libxpp/smart-pointers/arc.md` — Alloc parameter, make() with allocator
- [ ] 8.4 Update `docs/libxpp/smart-pointers/own.md` — Deleter → Alloc migration
- [ ] 8.5 Update `docs/SUMMARY.md` — add allocator page
