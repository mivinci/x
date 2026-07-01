## ADDED Requirements

### Requirement: PromiseNode poll returns Option<T>

`PromiseNode<T>::poll` SHALL return `Option<ValueType>`. `Some(value)` indicates the computation is ready and the value is available. `None` indicates the computation is pending and the waker has been stored for later notification. The `take()` method SHALL NOT exist.

#### Scenario: Immediate value
- **WHEN** an `ImmediatePromiseNode<T>` is polled
- **THEN** `poll` SHALL return `Some(value)` containing the stored value
- **AND** the waker SHALL NOT be stored

#### Scenario: Pending computation
- **WHEN** a node that is not yet ready is polled
- **THEN** `poll` SHALL return `None`
- **AND** the waker SHALL be stored for later notification

#### Scenario: Ready after wake
- **WHEN** a node's stored waker is fired and the node is re-polled
- **THEN** `poll` SHALL return `Some(value)`

### Requirement: Waker is a function pointer + arg

`Waker` SHALL be a simple struct containing a function pointer (`void (*)(void *)`) and a `void *` argument. `Waker::wake()` SHALL call the function pointer with the argument if the function pointer is non-null. `Waker` SHALL NOT depend on `Schedule`, `SpawnTaskBase`, or any virtual base class.

#### Scenario: Sync wait waker
- **WHEN** `Waker::sync_wait(&done, loop)` is called
- **THEN** the resulting waker, when fired, SHALL post a callback to `loop` that sets `*done = true`

#### Scenario: Null waker
- **WHEN** a default-constructed `Waker` is fired
- **THEN** it SHALL be a no-op (function pointer is null)

### Requirement: Promise wait blocks in WaitScope

`Promise<T>::wait()` SHALL block the calling thread until the promise resolves. It MUST be called inside a `WaitScope`. It SHALL use a local `bool done` flag (not a member variable). After `poll` returns `Some`, `wait()` SHALL return the value.

#### Scenario: Immediate resolve
- **WHEN** `Promise<int>::resolve(42).wait()` is called inside a WaitScope
- **THEN** `wait()` SHALL return 42 without running the event loop

#### Scenario: Deferred resolve
- **WHEN** a promise is created via `Promise::make()` and `wait()` is called
- **AND** the resolver is called from a timer callback on the same event loop
- **THEN** `wait()` SHALL run the event loop until the resolver fires, then return the resolved value

### Requirement: TransformPromiseNode chains poll

`TransformPromiseNode<U, T, Func>` SHALL poll its upstream node. If the upstream returns `Some(value)`, the transform SHALL apply `func` to the value and return `Some(result)`. If the upstream returns `None`, the transform SHALL return `None` (forwarding the waker).

#### Scenario: Transform ready
- **WHEN** upstream `poll` returns `Some(42)` and `func` is `[](int x) { return x * 2; }`
- **THEN** `TransformPromiseNode` poll SHALL return `Some(84)`

#### Scenario: Transform pending
- **WHEN** upstream `poll` returns `None`
- **THEN** `TransformPromiseNode` poll SHALL return `None`

### Requirement: ChainPromiseNode flattens nested promises

`ChainPromiseNode<T>` SHALL poll its outer node (`PromiseNode<Promise<T>>`). If the outer returns `Some(Promise<T>)`, the inner node is extracted and polled. If the inner returns `Some(value)`, that value is returned. If either outer or inner returns `None`, `None` is returned.

#### Scenario: Outer ready, inner ready
- **WHEN** the outer poll returns `Some(Promise<T>)` and the inner poll returns `Some(value)`
- **THEN** `ChainPromiseNode` SHALL return `Some(value)`

#### Scenario: Outer pending
- **WHEN** the outer poll returns `None`
- **THEN** `ChainPromiseNode` SHALL return `None`

### Requirement: AdapterPromiseNode supports external resolve

`AdapterPromiseNode<T>` SHALL support concurrent `poll` and `resolve`. `AtomicWaker` SHALL coordinate the waker registration and resolve notification without a mutex. `resolve(value)` SHALL store the value, set the resolved flag, and fire the waker. `poll(waker)` SHALL return `Some(value)` if resolved, or `None` (storing the waker) if not.

#### Scenario: Resolve before poll
- **WHEN** `resolve(value)` is called before `poll`
- **THEN** `poll` SHALL return `Some(value)` immediately

#### Scenario: Poll before resolve
- **WHEN** `poll` is called (returns `None`), then `resolve(value)` is called
- **THEN** the stored waker SHALL be fired
- **AND** the next `poll` SHALL return `Some(value)`

### Requirement: Removed SpawnTaskBase and Schedule

The types `SpawnTaskBase`, `Schedule`, `SyncWaitSchedule`, and `CoroWakeSchedule` SHALL NOT exist in the codebase. `Waker` SHALL NOT reference `SpawnTaskBase`.

#### Scenario: No SpawnTaskBase references
- **WHEN** the codebase is searched for `SpawnTaskBase`
- **THEN** zero matches SHALL be found in `libxpp/`

### Requirement: No coroutine support

The `#if XPP_HAS_COROUTINES` blocks SHALL be removed from `promise.h` and `promise_node.h`. No `promise_type`, `operator co_await`, `CoroWakeSchedule`, or `#include <coroutine>` SHALL exist.

#### Scenario: No coroutine references
- **WHEN** the codebase is searched for `coroutine` or `co_await`
- **THEN** zero matches SHALL be found in `libxpp/`
