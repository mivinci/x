## ADDED Requirements

### Requirement: WeakResolver safe after Promise destruction
WeakResolver SHALL hold an ArcWeak to the shared ResolveState. Calling resolve() after the Promise (and AdaptedPromiseNode) is destroyed SHALL silently drop — no crash, no side effect.

#### Scenario: Promise destroyed before resolve
- **WHEN** a Promise backed by AdaptedPromiseNode is destroyed, and WeakResolver::resolve() is called afterward
- **THEN** resolve() SHALL call ArcWeak::upgrade(), receive None, and return without side effect

#### Scenario: Cross-thread resolve after destruction
- **WHEN** a worker thread calls WeakResolver::resolve() while the event loop thread destroys the Promise
- **THEN** exactly one of the two outcomes SHALL occur: (a) upgrade succeeds → value set + waker fired, or (b) upgrade fails → silently dropped. No UAF.

#### Scenario: Double resolve is ignored
- **WHEN** resolve() is called twice
- **THEN** only the first call SHALL set the value; the second SHALL be silently dropped (atomic flag check)

### Requirement: WeakResolver thread-safe resolve
WeakResolver::resolve() SHALL be callable from any thread. The ArcWeak::upgrade() CAS loop guarantees safe concurrent access.

#### Scenario: Concurrent resolve from multiple threads
- **WHEN** two threads call resolve() simultaneously
- **THEN** exactly one SHALL succeed (the one that wins the resolved flag CAS); the other SHALL drop

### Requirement: newPromiseAndResolver factory
The factory SHALL return a Promise<T> and a WeakResolver<T> that share the same ResolveState.

#### Scenario: Manual resolve
- **WHEN** `auto [p, r] = newPromiseAndResolver<int>()` is called
- **THEN** p.wait() SHALL block until r.resolve(value) is called, then return value

## ADDED Requirements

### Requirement: Adapter contract
An Adapter SHALL be a class with a constructor accepting `WeakResolver<T>&&` followed by user-supplied arguments. The constructor starts the async operation. The destructor cancels it. The async callback calls `WeakResolver::resolve()`.

#### Scenario: TimerAdapter
- **WHEN** `newAdaptedPromise<void, TimerAdapter>(ms)` is called
- **THEN** TimerAdapter's constructor SHALL call xTimerStart, and its destructor SHALL call xTimerStop

#### Scenario: Adapter destructor cancels operation
- **WHEN** an AdaptedPromiseNode is destroyed while the async operation is in-flight
- **THEN** the Adapter's destructor SHALL run and cancel the operation (e.g., stop timer, close socket)

### Requirement: AdaptedPromiseNode generic poll
AdaptedPromiseNode SHALL implement poll() generically: check resolved flag, register waker if not resolved, return value if resolved. No per-Adapter poll logic.

#### Scenario: Poll before resolve
- **WHEN** poll() is called before resolve()
- **THEN** AdaptedPromiseNode SHALL register the waker with ResolveState and return None

#### Scenario: Poll after resolve
- **WHEN** poll() is called after resolve()
- **THEN** AdaptedPromiseNode SHALL return Some(value) from ResolveState

### Requirement: newAdaptedPromise factory
The factory SHALL create an AdaptedPromiseNode with the user-supplied Adapter type and return a Promise<T>.

#### Scenario: Custom adapter
- **WHEN** `newAdaptedPromise<T, MyAdapter>(args...)` is called
- **THEN** MyAdapter SHALL be constructed with `(WeakResolver<T>&&, args...)`, and the returned Promise<T> SHALL resolve when MyAdapter calls resolve()
