## ADDED Requirements

### Requirement: TimerAdapter replaces TimerPromiseNode
TimerAdapter SHALL be a thin Adapter class that manages an xTimer handle. It replaces TimerPromiseNode, moving poll/waker/resolved logic to AdaptedPromiseNode.

#### Scenario: after(ms) still works
- **WHEN** `Promise<void>::after(100).then(fn).wait()` is called
- **THEN** the promise SHALL resolve after ~100ms, using AdaptedPromiseNode + TimerAdapter internally

#### Scenario: Timer cancelled on early destruction
- **WHEN** an AdaptedPromiseNode<void, TimerAdapter> is destroyed before the timer fires
- **THEN** TimerAdapter's destructor SHALL call xTimerStop, and the timer callback SHALL NOT fire

#### Scenario: Timer fires and resolves
- **WHEN** the timer fires
- **THEN** the callback SHALL call WeakResolver::resolve(), which sets resolved=true and wakes the poller

## REMOVED Requirements

### Requirement: TimerPromiseNode
**Reason**: Replaced by TimerAdapter + AdaptedPromiseNode. The poll/waker/resolved logic is now centralized in AdaptedPromiseNode; TimerAdapter only manages the xTimer handle.
**Migration**: `Promise<void>::after(ms)` now uses `newAdaptedPromise<void, TimerAdapter>(ms)` internally. Public API unchanged.
