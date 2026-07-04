## Why

Every `Promise::then()` call heap-allocates a `PromiseNode` via `::operator new`. A chain of N `.then()` calls makes N individual malloc calls, each for a small object (24–64B) where malloc metadata overhead is proportionally high. KJ (Cap'n Proto) solves this with a per-chain bump arena — one allocation serves the entire chain. We should do the same.

## What Changes

- Add `Arena<N>`: a general-purpose bump allocator template with compile-time size parameter. Small N (≤256B) stores the buffer inline (zero malloc); large N (>256B) heap-allocates the buffer (one malloc). No chunk-list growth — `allocate()` returns `nullptr` when full, caller falls back to heap.
- Add `DynamicArena`: a runtime-sized variant for cases where the size isn't known at compile time. One heap allocation for the buffer.
- Add `PromiseArena` (`Arena<256>`): the specific instantiation used for PromiseNode chains. Embedded in the chain's tail node, bump-allocates nodes backwards (or forwards), freed when the chain is destroyed.
- Modify `PromiseNode` allocation: `.then()` chains allocate new nodes in the existing arena (from the predecessor's arena) instead of individual `new` calls. Oversized nodes or missing arena fall back to heap.
- Add `arena_test.cpp`: tests for allocation, alignment, overflow, reset, `owns()`, inline vs heap storage.

## Capabilities

### New Capabilities
- `arena`: Bump allocator (`Arena<N>` and `DynamicArena`) for short-lived objects. Inline storage for small sizes, heap for large. `allocate()`, `owns()`, `reset()`, `remaining()`.

### Modified Capabilities
- `promise`: PromiseNode allocation changes from per-node `new` to per-chain arena bump. Transparent to users — no API change to `Promise<T>`. Reduces malloc calls from O(N) to O(1) per `.then()` chain.

## Impact

- **New files**: `libxpp/xpp/arena.h`, `libxpp/xpp/arena_test.cpp`
- **Modified files**: `libxpp/xpp/promise_node.h` (PromiseNode base gets arena support), `libxpp/xpp/promise.h` (then/chain allocation path), `libxpp/xpp/promise_adapter.h`, `libxpp/xpp/promise_combinators.h`, `libxpp/xpp/promise_coroutine.h`
- **No breaking API changes**: `Promise<T>` public API unchanged. Arena is internal optimization.
- **Dependencies**: None new. `Arena<N>` is pure C++11, no libx dependency.
- **Docs**: New `docs/libxpp/arena.md` page; update `docs/SUMMARY.md`.
