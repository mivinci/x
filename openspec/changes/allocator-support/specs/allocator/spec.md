## ADDED Requirements

### Requirement: Allocator protocol
An Allocator SHALL provide `allocate(Layout) → Result<AllocInfo, AllocError>` and `deallocate(void*, Layout)`. It MAY provide `grow(void*, Layout, Layout)` and `shrink(void*, Layout, Layout)` for optimization; defaults are allocate + copy + deallocate.

#### Scenario: Successful allocation
- **WHEN** `alloc.allocate(Layout::of<int>())` is called
- **THEN** it SHALL return `Result` with `AllocInfo{ptr != null, size >= 4}`

#### Scenario: Allocation failure
- **WHEN** allocation fails (out of memory)
- **THEN** it SHALL return `Result` with `AllocError`

#### Scenario: Deallocation
- **WHEN** `alloc.deallocate(ptr, layout)` is called with a valid pointer from `allocate`
- **THEN** the memory SHALL be freed

#### Scenario: Default grow
- **WHEN** an Allocator without custom `grow` is grown
- **THEN** the default implementation SHALL allocate new, copy old data, deallocate old

### Requirement: GlobalAllocator
`GlobalAllocator` SHALL be the default allocator for all smart pointers. It SHALL be an empty class (EBO-eligible). It SHALL use `::operator new` / `::operator delete` (C++17 aligned variants when available).

#### Scenario: Empty class
- **WHEN** `sizeof(GlobalAllocator)` is evaluated
- **THEN** it SHALL be 1 (empty class), but EBO-eligible (0 bytes in CompressedPair/ArcInner)

### Requirement: Layout struct
`Layout` SHALL bundle `size` and `align`. `Layout::of<T>()` SHALL return `{sizeof(T), alignof(T)}`.

#### Scenario: Layout of int
- **WHEN** `Layout::of<int>()` is called
- **THEN** it SHALL return `{4, 4}` (or platform-appropriate values)

## MODIFIED Requirements

### Requirement: Arc with Allocator
`Arc<T, Alloc>` SHALL store `Alloc` in `ArcInner`. `sizeof(Arc<T, Alloc>)` SHALL be `sizeof(T*)` regardless of `Alloc`. `make()` SHALL accept optional allocator argument.

#### Scenario: Default allocator
- **WHEN** `Arc<int>::make(42)` is called
- **THEN** `sizeof(Arc<int>) == sizeof(int*)` and `GlobalAllocator` is used

#### Scenario: Stateful allocator
- **WHEN** `Arc<int, ArenaAlloc>::make(arena, 42)` is called
- **THEN** `sizeof(Arc<int, ArenaAlloc>) == sizeof(int*)` and `ArenaAlloc` is stored in `ArcInner`

### Requirement: Own with Allocator (replaces Deleter)
`Own<T, Alloc>` SHALL use `Alloc::deallocate` instead of `Deleter::operator()`. Destruction SHALL call `~T()` then `Alloc::deallocate`. `sizeof(Own<T>) == sizeof(T*)` when `Alloc` is empty (EBO via CompressedPair).

#### Scenario: Default allocator
- **WHEN** `Own<int>(new int(42))` is destroyed
- **THEN** `~int()` is called, then `GlobalAllocator::deallocate(ptr, Layout::of<int>())`

#### Scenario: Custom allocator
- **WHEN** `Own<int, ArenaAlloc>(arena, 42)` is destroyed
- **THEN** `~int()` is called, then `ArenaAlloc::deallocate(ptr, Layout::of<int>())`
