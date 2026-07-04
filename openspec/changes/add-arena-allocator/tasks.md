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

- [ ] 3.1 Add `PromiseArena` typedef (`Arena<256>`) and arena pointer to `PromiseNode` base
- [ ] 3.2 Add `virtual destroy()` method to `PromiseNode` (for arena-aware destruction without `operator delete`)
- [ ] 3.3 Add `PromiseDisposer` (or equivalent) that checks `arena.owns(ptr)` before `::operator delete`
- [ ] 3.4 Update `Own<PromiseNode<T>>` to use `PromiseDisposer` instead of default `delete`
- [ ] 3.5 Update `.then()` allocation path: try arena bump from predecessor's arena, fall back to `new`
- [ ] 3.6 Update arena ownership transfer: new tail node takes arena pointer, old node nulls it
- [ ] 3.7 Update `resolve()` / `async()` / `adapt()` / `work()` / `after()` / `yield()`: first node uses heap (no arena), arena starts at first `.then()`
- [ ] 3.8 Update `all()` / `race()`: nodes use heap (not part of a `.then()` chain)
- [ ] 3.9 Update coroutine `CoroutinePromiseNode`: uses heap (coroutine frame is its own allocation)

## 4. PromiseNode tests

- [ ] 4.1 Regression: all existing promise tests pass unchanged
- [ ] 4.2 Arena hit: `.then()` chain of 5+ small nodes uses 1 arena allocation (verify via counting allocator)
- [ ] 4.3 Arena overflow: chain exceeding 256B falls back to heap, still works
- [ ] 4.4 Node destruction: arena-owned nodes skip `::operator delete`, heap nodes don't
- [ ] 4.5 Coroutine: `co_await` chains still work (heap path, no arena)
- [ ] 4.6 Combinators: `all()` / `race()` still work (heap path)

## 5. Docs

- [ ] 5.1 Create `docs/libxpp/arena.md` — Arena<N> API, inline vs heap, owns(), reset(), PromiseNode usage
- [ ] 5.2 Update `docs/SUMMARY.md` — add Arena page
- [ ] 5.3 Update `docs/libxpp/README.md` — add Arena to module list
