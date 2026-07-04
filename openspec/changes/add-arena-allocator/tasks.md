## 1. Arena<N> core (arena.h)

- [x] 1.1 Define `ArenaStorage<N, bool Inline>` primary template (inline specialization with `alignas(max_align_t) char buf[N]`, heap specialization with `char* ptr` + constructor/destructor)
- [x] 1.2 Define `Arena<N>` class: `ArenaStorage<N> m_storage`, `char* m_pos`, `char* m_end`
- [x] 1.3 Implement `allocate(size_t size, size_t align)` — bump with `align_up`, return `nullptr` on overflow
- [x] 1.4 Implement `owns(const void* p)` — pointer range check
- [x] 1.5 Implement `reset()` — `m_pos = m_storage.begin()`
- [x] 1.6 Implement `make<T>(args...)` — allocate + placement new
- [x] 1.7 Implement queries: `capacity()`, `remaining()`, `used()`
- [x] 1.8 Add `static_assert` verifying `sizeof(Arena<N>)` for inline vs heap cases
- [x] 1.9 Add `align_up` helper (round pointer up to alignment)

## 2. Arena tests (arena_test.cpp)

- [x] 2.1 Basic allocation: allocate within capacity, check pointer and remaining
- [x] 2.2 Custom alignment: allocate with alignof values, verify alignment
- [x] 2.3 Overflow: fill arena, verify nullptr on next allocate
- [x] 2.4 Contiguity: multiple allocations are adjacent (modulo alignment)
- [x] 2.5 owns(): true for arena pointers, false for heap pointers
- [x] 2.6 reset(): clears arena, re-allocate works
- [x] 2.7 make<T>(): construct object, verify value and ownership
- [x] 2.8 Inline storage: `sizeof(Arena<128>)` includes buffer, `sizeof(Arena<4096>)` does not
- [x] 2.9 Capacity queries: `capacity()`, `remaining()`, `used()` correct before/after alloc

## 3. PromiseNode arena integration

- [x] 3.1 Add `PromiseArena` typedef (`Arena<256>`) and `PromiseNodeAllocator` (custom Allocator with header-based routing)
- [x] 3.2 ~~Add `virtual destroy()` method~~ — not needed; header-based `PromiseNodeAllocator::deallocate` handles routing
- [x] 3.3 `PromiseNodeAllocator::deallocate` reads arena pointer from 8B header: null=heap (free), non-null=arena (skip)
- [x] 3.4 `Own<PromiseNode<T>>` → `Own<PromiseNode<T>, PromiseNodeAllocator>` (via `OwnPromiseNode<T>` typedef)
- [x] 3.5 `.then()` creates/reuses arena, calls `allocate_promise<T>(arena, ...)` — arena bump or heap fallback
- [x] 3.6 Arena ownership: `Promise<T>` owns `m_arena`, transfers to child on `.then()`. Declared before `m_node` so destroyed after (reverse declaration order).
- [x] 3.7 `resolve()` / `async()` / `adapt()` / `work()` / `after()` / `yield()`: first node uses `allocate_promise(nullptr, ...)` (heap, no arena)
- [x] 3.8 `all()` / `race()`: nodes use `allocate_promise(nullptr, ...)` (heap)
- [x] 3.9 `CoroutinePromiseNode`: uses `allocate_promise(nullptr, ...)` (heap)

## 4. PromiseNode tests

- [x] 4.1 Regression: all existing promise tests pass unchanged (42+8+17+14 = 81 tests)
- [ ] 4.2 Arena hit: `.then()` chain of 5+ small nodes uses 1 arena allocation (verify via counting allocator)
- [ ] 4.3 Arena overflow: chain exceeding 256B falls back to heap, still works
- [ ] 4.4 Node destruction: arena-owned nodes skip `::operator delete`, heap nodes don't
- [x] 4.5 Coroutine: `co_await` chains still work (heap path, no arena) — 14 tests pass
- [x] 4.6 Combinators: `all()` / `race()` still work (heap path) — 17 tests pass

## 5. Docs

- [x] 5.1 Create `docs/libxpp/arena.md` — Arena<N> API, inline vs heap, owns(), reset(), PromiseNode usage
- [x] 5.2 Update `docs/SUMMARY.md` — add Arena page
- [x] 5.3 Update `docs/libxpp/README.md` — add Arena to module list
