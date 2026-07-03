## ADDED Requirements

### Requirement: all with heterogeneous types
`xpp::all` SHALL accept a variadic pack of `Promise<Ts>...` with different types and return `Promise<std::tuple<FixVoid<Ts>::Type...>>` that resolves when all inputs have resolved.

#### Scenario: Mixed int and string
- **WHEN** `xpp::all(Promise<int>::resolve(42), Promise<std::string>::resolve("hi"))` is waited
- **THEN** the result SHALL be a `std::tuple<int, std::string>` with `std::get<0> == 42` and `std::get<1> == "hi"`

#### Scenario: With void promise
- **WHEN** `xpp::all(Promise<void>::resolve(), Promise<int>::resolve(7))` is waited
- **THEN** the result SHALL be `std::tuple<Void, int>` where `std::get<1> == 7`

#### Scenario: Deferred resolution
- **WHEN** inputs are backed by `PromiseResolver` and resolve after a delay
- **THEN** `all` SHALL not resolve until all inputs have resolved; the waker fires on each child completion, re-polling until all are done

### Requirement: all with all void types
`xpp::all` SHALL return `Promise<void>` (not `Promise<tuple<Void, Void, ...>>`) when all input promises are `Promise<void>`.

#### Scenario: Three void promises
- **WHEN** `xpp::all(Promise<void>::resolve(), Promise<void>::resolve(), Promise<void>::resolve())` is waited
- **THEN** the result SHALL be `Promise<void>` that resolves when all three inputs resolve

#### Scenario: Deferred void promises with timer
- **WHEN** `xpp::all(Promise<void>::after(10), Promise<void>::after(50))` is waited
- **THEN** the promise SHALL resolve after approximately 50ms (the slower of the two)

### Requirement: all zero-argument rejection
`xpp::all()` with zero arguments SHALL fail to compile.

#### Scenario: Zero arguments
- **WHEN** `xpp::all()` is written
- **THEN** a `static_assert` SHALL produce a readable error message

### Requirement: all one-shot polling
`xpp::all` SHALL poll each child at most once after it returns `Some`. Children that have already resolved SHALL be skipped on subsequent polls.

#### Scenario: Child resolves early
- **WHEN** child 0 resolves immediately but child 1 is deferred
- **THEN** on re-poll (triggered by child 1's waker), child 0 SHALL NOT be polled again; only child 1 is polled
