## MODIFIED Requirements

### Requirement: PromiseNode allocation via per-chain arena
PromiseNode objects in a `.then()` chain are bump-allocated from a per-chain `PromiseArena` (`Arena<256>`) when they fit. Oversized nodes or missing arena fall back to `::operator new`. The arena is owned by the chain's tail node and freed when the tail is destroyed.

#### Scenario: then() chain uses arena
- **WHEN** `p.then(f1).then(f2).then(f3)` is called
- **THEN** at most 1 heap allocation occurs for the arena (plus 1 for the first node if it doesn't have an arena); subsequent nodes are bump-allocated in the same arena if they fit

#### Scenario: Arena overflow falls back to heap
- **WHEN** a `.then()` chain exceeds 256 bytes of nodes
- **THEN** excess nodes are allocated via `::operator new` (heap fallback), and the chain still works correctly

#### Scenario: Node destruction routes correctly
- **WHEN** a PromiseNode is destroyed
- **THEN** if the node's memory is owned by an arena, no `::operator delete` is called (arena bulk-frees); if heap-allocated, `::operator delete` is called

#### Scenario: Transparent to users
- **WHEN** any `Promise<T>` API is used (`then`, `wait`, `co_await`, `all`, `race`, etc.)
- **THEN** behavior is identical to before — arena is an internal optimization with no API change
