## ADDED Requirements

### Requirement: race returns first resolved
`xpp::race` SHALL accept a variadic pack of homogeneous `Promise<T>` and return `Promise<T>` that resolves with the first input to resolve, discarding all others.

#### Scenario: Immediate winner
- **WHEN** `xpp::race(Promise<int>::resolve(1), Promise<int>::resolve(2))` is waited
- **THEN** the result SHALL be `1` (first argument, first to resolve)

#### Scenario: Deferred winner
- **WHEN** `race(Promise<int>::after(50).then([]{return 1;}), Promise<int>::after(10).then([]{return 2;}))` is waited
- **THEN** the result SHALL be `2` (the faster timer wins)

#### Scenario: One immediate, one deferred
- **WHEN** `race(Promise<int>::resolve(42), deferred_promise)` is waited
- **THEN** the result SHALL be `42` and the deferred promise's node SHALL be destroyed without being polled again

### Requirement: race homogeneous types only
`xpp::race` SHALL reject heterogeneous types at compile time.

#### Scenario: Mixed types
- **WHEN** `xpp::race(Promise<int>::resolve(1), Promise<std::string>::resolve("hi"))` is written
- **THEN** a `static_assert` SHALL produce a readable error message

### Requirement: race void specialization
`xpp::race` SHALL support `Promise<void>` inputs, returning `Promise<void>` that resolves when the first input resolves.

#### Scenario: Timer race (timeout pattern)
- **WHEN** `race(Promise<void>::after(1000), Promise<void>::after(50))` is waited
- **THEN** the promise SHALL resolve after approximately 50ms

### Requirement: race destruction of losing branches
When race resolves, all non-winning child nodes SHALL be destroyed (their destructors called).

#### Scenario: TimerPromiseNode cleanup
- **WHEN** `race(immediate_promise, Promise<void>::after(5000))` resolves immediately
- **THEN** the `TimerPromiseNode` from `after(5000)` SHALL be destroyed and its timer SHALL be stopped via `xTimerStop`
