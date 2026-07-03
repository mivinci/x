## ADDED Requirements

### Requirement: Promise<T> as coroutine return type
`Promise<T>` SHALL have a `promise_type` nested type (when C++20 coroutines are available) so that functions returning `Promise<T>` can be coroutines using `co_return`.

#### Scenario: Simple coroutine
- **WHEN** a function `Promise<int> foo() { co_return 42; }` is defined and `foo().wait()` is called
- **THEN** the result SHALL be `42`

#### Scenario: Coroutine with co_await
- **WHEN** a coroutine `co_await`s a `Promise<U>` and the awaited promise resolves
- **THEN** the coroutine SHALL resume and the `co_await` expression SHALL yield the resolved value

#### Scenario: Coroutine returning void
- **WHEN** a function `Promise<void> foo() { co_return; }` is defined
- **THEN** `foo().wait()` SHALL complete without error

### Requirement: co_await Promise<T>
`Promise<T>` SHALL be awaitable via `operator co_await()` (rvalue-qualified) inside any coroutine. The awaiter extracts the PromiseNode, stores it in the current coroutine's CoroutinePromiseNode for polling, and suspends.

#### Scenario: Await resolved promise
- **WHEN** `co_await Promise<int>::resolve(42)` is evaluated
- **THEN** the coroutine SHALL suspend, the node SHALL be polled, and the result `42` SHALL be returned by `await_resume()`

#### Scenario: Await deferred promise
- **WHEN** `co_await` is used on a promise that resolves later (e.g., `Promise<void>::after(50)`)
- **THEN** the coroutine SHALL suspend, the waker SHALL fire when the promise resolves, `poll()` SHALL be called again, and the coroutine SHALL resume

#### Scenario: Await in sequence
- **WHEN** a coroutine `co_await`s multiple promises in sequence
- **THEN** each `co_await` SHALL suspend and resume in order, with no interference between them

### Requirement: CoroutinePromiseNode drives execution
`CoroutinePromiseNode<T>` SHALL implement `PromiseNode<T>::poll()` to drive the coroutine: resume on first call, poll any awaited promise on subsequent calls, and return `Some(result)` when the coroutine `co_returns`.

#### Scenario: First poll starts coroutine
- **WHEN** `poll()` is called for the first time
- **THEN** the coroutine SHALL be resumed (started), and if it immediately `co_returns`, `Some(result)` SHALL be returned

#### Scenario: Coroutine awaits and resumes
- **WHEN** the coroutine is suspended on `co_await` and `poll()` is called
- **THEN** the awaited promise SHALL be polled; if ready, the coroutine SHALL be resumed; if not ready, `None` SHALL be returned

### Requirement: C++17 compatibility
All coroutine code SHALL be behind `#if XPP_HAS_COROUTINES`. C++17 compilation SHALL produce no coroutine-related code or includes.

#### Scenario: C++17 build
- **WHEN** the project is compiled with C++17
- **THEN** `Promise<T>` SHALL NOT have `promise_type` or `operator co_await`, and `<coroutine>` SHALL NOT be included

### Requirement: Coroutine cleanup on early destruction
When a `Promise<T>` produced by a coroutine is destroyed before the coroutine completes, the coroutine frame SHALL be destroyed safely.

#### Scenario: Promise destroyed while coroutine suspended
- **WHEN** a coroutine is suspended on `co_await` and the `Promise<T>` is destroyed
- **THEN** `CoroutinePromiseNode`'s destructor SHALL call `handle.destroy()`, and the awaited promise's node SHALL be destroyed (cancelling it)
