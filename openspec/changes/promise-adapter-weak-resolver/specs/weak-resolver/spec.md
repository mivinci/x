## ADDED Requirements

### Requirement: WeakResolver replaces PromiseResolver
WeakResolver<T> SHALL replace PromiseResolver<T> as the public API for manual promise resolution. It holds an ArcWeak to shared state, making it safe to call after the Promise is destroyed.

#### Scenario: Migration from PromiseResolver
- **WHEN** existing code uses `PromiseResolver<int>::create()` + `.promise()` + `.resolve(v)`
- **THEN** it SHALL be replaced with `newPromiseAndResolver<int>()` returning `{Promise, WeakResolver}`, and `WeakResolver::resolve(v)`

#### Scenario: is_pending check
- **WHEN** `WeakResolver::is_pending()` is called
- **THEN** it SHALL return true if the Promise is alive and not yet resolved, false otherwise

### Requirement: WeakResolver void specialization
WeakResolver<void> SHALL support resolve() with no arguments, matching Promise<void>.

#### Scenario: Void resolve
- **WHEN** `WeakResolver<void>::resolve()` is called
- **THEN** the associated Promise<void> SHALL resolve (if still alive)
