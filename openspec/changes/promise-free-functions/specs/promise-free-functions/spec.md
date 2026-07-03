## ADDED Requirements

### Requirement: Free function factories
All Promise factory functions SHALL be free functions in namespace `xpp`, supporting type deduction where possible.

#### Scenario: resolve with value
- **WHEN** `xpp::resolve(42)` is called
- **THEN** it SHALL return `Promise<int>` with T deduced from the argument

#### Scenario: after delay
- **WHEN** `xpp::after(100)` is called
- **THEN** it SHALL return `Promise<void>` that resolves after 100ms

#### Scenario: defer function
- **WHEN** `xpp::defer([] { return 42; })` is called
- **THEN** it SHALL return `Promise<int>` with T deduced from the function return type

#### Scenario: work function
- **WHEN** `xpp::work([] { return 42; })` is called
- **THEN** it SHALL return `Promise<int>` with T deduced from the function return type, submitted to thread pool

#### Scenario: adapt with explicit types
- **WHEN** `xpp::adapt<int, MyAdapter>(args...)` is called
- **THEN** it SHALL return `Promise<int>` backed by `AdapterPromiseNode<int, MyAdapter>`

### Requirement: Static factory methods removed
`Promise<T>::resolve()`, `Promise<void>::after()`, `Promise<T>::defer()`, `Promise<T>::work()`, `Promise<T>::adapt()` SHALL be removed. No deprecated wrappers. `Promise<T>` is a pure consumer type.

#### Scenario: Static resolve removed
- **WHEN** code uses `Promise<int>::resolve(42)`
- **THEN** it SHALL NOT compile — must use `xpp::resolve(42)`
