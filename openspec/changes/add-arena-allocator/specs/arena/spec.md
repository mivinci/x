## ADDED Requirements

### Requirement: Arena bump allocation
Arena<N> allocates memory by bumping a pointer. Memory is never individually freed — only bulk-freed via arena destruction or reset.

#### Scenario: Allocate within capacity
- **WHEN** `Arena<256> a; void* p = a.allocate(64);`
- **THEN** `p` is non-null, aligned to `max_align_t`, and `a.remaining() == 256 - 64` (accounting for alignment)

#### Scenario: Allocate with custom alignment
- **WHEN** `Arena<256> a; void* p = a.allocate(32, 64);`
- **THEN** `p` is 64-byte aligned

#### Scenario: Overflow returns nullptr
- **WHEN** `Arena<64> a; void* p1 = a.allocate(48); void* p2 = a.allocate(32);`
- **THEN** `p1` is non-null, `p2` is `nullptr` (not enough space)

#### Scenario: Multiple allocations are contiguous
- **WHEN** `Arena<256> a; void* p1 = a.allocate(16); void* p2 = a.allocate(16);`
- **THEN** `p2 == (char*)p1 + 16` (or aligned equivalent)

### Requirement: Inline storage for small arenas
Arena<N> with N ≤ 256 stores the buffer inline (as a member), requiring zero heap allocations.

#### Scenario: Small arena has no heap allocation
- **WHEN** `Arena<128> a;` is constructed on the stack
- **THEN** no `::operator new` is called, and `sizeof(Arena<128>)` includes the 128-byte buffer

### Requirement: Heap storage for large arenas
Arena<N> with N > 256 heap-allocates the buffer at construction. sizeof(Arena<N>) is small (3 pointers + 1 pointer for storage).

#### Scenario: Large arena uses heap
- **WHEN** `Arena<4096> a;` is constructed
- **THEN** one `::operator new(4096)` is called, and `sizeof(Arena<4096>)` does not include 4096 bytes

### Requirement: owns() pointer ownership check
Arena can determine whether a pointer was allocated from it, in O(1) time.

#### Scenario: Pointer from arena
- **WHEN** `Arena<256> a; void* p = a.allocate(32);`
- **THEN** `a.owns(p)` is `true`

#### Scenario: Pointer from heap
- **WHEN** `Arena<256> a; void* p = ::operator new(32);`
- **THEN** `a.owns(p)` is `false`

### Requirement: reset() clears without freeing
reset() resets the bump pointer to the start, keeping the buffer allocated for reuse.

#### Scenario: Reset then re-allocate
- **WHEN** `Arena<256> a; a.allocate(128); a.reset(); void* p = a.allocate(128);`
- **THEN** `p` is non-null and `a.remaining() == 128`

### Requirement: make<T>() typed allocation
Arena provides a `make<T>(args...)` helper that allocates and constructs an object in-place.

#### Scenario: Construct object in arena
- **WHEN** `Arena<256> a; auto* p = a.make<std::string>("hello");`
- **THEN** `*p == "hello"` and `a.owns(p)` is `true`

### Requirement: Capacity queries
Arena provides `capacity()`, `remaining()`, and `used()` queries.

#### Scenario: Fresh arena
- **WHEN** `Arena<128> a;`
- **THEN** `a.capacity() == 128`, `a.remaining() == 128`, `a.used() == 0`

#### Scenario: After allocation
- **WHEN** `Arena<128> a; a.allocate(32);`
- **THEN** `a.used() >= 32`, `a.remaining() <= 96`
