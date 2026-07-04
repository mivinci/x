## Context

Every `Promise::then()` call allocates a `PromiseNode` (24–64B) via `::operator new`. A chain of N `.then()` calls makes N individual malloc calls. For high-frequency promise chains (HTTP server, event-driven pipelines), this malloc overhead is significant.

KJ (Cap'n Proto) solves this with `PromiseArena` — a 1024-byte bump allocator attached to the chain's tail node. New nodes are bump-allocated in the same arena; the arena is freed when the chain is destroyed. This reduces O(N) malloc to O(1).

Our PromiseNodes are smaller than KJ's (24–64B vs 64–128B), so a 256-byte arena fits 5–10 typical nodes — enough for most `.then()` chains.

## Goals / Non-Goals

**Goals:**
- `Arena<N>`: general-purpose bump allocator, template size parameter
- Small N (≤256B): inline buffer, zero heap allocation
- Large N (>256B): heap-allocated buffer via `ArenaStorage` specialization, one allocation
- `allocate()` returns `nullptr` when full — caller falls back to heap
- `owns(p)`: O(1) check if pointer is in arena (for dealloc routing)
- `reset()`: mark all memory as free, keep buffer for reuse
- PromiseNode: `.then()` chains use arena bump allocation, transparent to users

**Non-Goals:**
- Chunk-list growth (arena is fixed-size, no auto-grow)
- Destructor tracking (caller responsible for `~T()`)
- Thread-safe arena (single-thread, like xSlab)
- Runtime-sized `Arena` (template only; `DynamicArena` is a separate class if needed later)
- Reducing node count (fusing `.then()` chains — that's a coroutine concern)
- Changing `Promise<T>` public API

## Decisions

### D1: Template size parameter with storage specialization

```cpp
template <size_t N, bool Inline = (N <= 256)>
struct ArenaStorage;

// Inline: buffer is a member, sizeof = N + overhead
template <size_t N>
struct ArenaStorage<N, true> {
    alignas(max_align_t) char buf[N];
    char* begin() const { return const_cast<char*>(buf); }
};

// Heap: buffer is heap-allocated, sizeof = 1 pointer + overhead
template <size_t N>
struct ArenaStorage<N, false> {
    char* ptr;
    ArenaStorage() : ptr(static_cast<char*>(::operator new(N))) {}
    ~ArenaStorage() { ::operator delete(ptr); }
    char* begin() const { return ptr; }
};

template <size_t N>
class Arena {
    ArenaStorage<N> m_storage;
    char* m_pos;
    char* m_end;
    // ... allocate/owns/reset/remaining
};
```

**Rationale**: Template parameter gives exact sizing — no inline buffer waste for large arenas. `ArenaStorage` specialization handles inline vs heap transparently; `Arena<N>` code is identical for both.

### D2: 256B threshold for inline

PromiseNode typical size: 24–48B. 256B fits 5–10 nodes — enough for a typical `.then()` chain. Larger arenas (e.g., 4KB) automatically use heap storage (24B object + 1 malloc).

### D3: allocate() returns nullptr on overflow

```cpp
void* allocate(size_t size, size_t align) {
    char* p = align_up(m_pos, align);
    if (p + size > m_end) return nullptr;
    m_pos = p + size;
    return p;
}
```

Caller checks return value and falls back to `::operator new`. This is simpler than auto-growing (no chunk list) and makes `owns()` trivial (pointer range check).

### D4: PromiseNode arena ownership model (mirrors KJ)

- Each `.then()` chain has at most one `PromiseArena` (typedef `Arena<256>`)
- Arena is owned by the **tail node** (the most recently appended)
- When a new node is appended via `.then()`:
  - If predecessor has an arena and it fits → bump-allocate in predecessor's arena, transfer ownership to new node
  - If no arena or doesn't fit → `::operator new` (heap fallback), new node has no arena
- When tail node is destroyed: `delete arena` (frees all chain memory in one call)
- `owns()` check in dealloc: if pointer is in arena, skip `::operator delete`

### D5: No destructor tracking

Arena does not call `~T()`. PromiseNode has its own `destroy()` virtual method (like KJ). The arena only manages raw memory. This keeps the arena general-purpose and zero-overhead.

## Risks / Trade-offs

- **[Short chain waste]** A 1-node chain with arena wastes 256B. Mitigated by: only create arena on `.then()` (not on `resolve()`). First node uses heap; arena starts at first `.then()`.
- **[Fixed size]** Arena can't grow. Long chains that overflow 256B fall back to heap for excess nodes. Acceptable — long chains are rare, and heap fallback is correct (just not optimal).
- **[Non-movable]** `Arena<N>` with inline storage can't be safely moved (buffer is part of object). Acceptable — arena is created in place (embedded in PromiseNode or heap-allocated), never moved.
- **[Complexity]** Arena ownership transfer in PromiseNode adds complexity to `.then()` path. Mitigated by: fallback to heap is always correct; arena is an optimization, not a correctness requirement.
