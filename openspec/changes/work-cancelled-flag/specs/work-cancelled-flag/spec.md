# work-cancelled-flag

## ADDED Requirements

### Requirement: xErrno_InProgress error code

The system SHALL provide `xErrno_InProgress` as a distinct error code indicating an operation cannot be cancelled because it is already being executed.

#### Scenario: Task already running
- **WHEN** `xTaskCancel` is called on a task that has been dequeued and is executing
- **THEN** the function returns `xErrno_InProgress`

### Requirement: done_fn not called after cancel

The system SHALL NOT invoke `done_fn` for a work item after `xWorkCancel` has been called, regardless of whether the underlying task was dequeued or already executing.

#### Scenario: Cancel before task starts
- **WHEN** `xWorkCancel` is called on a pending work item
- **THEN** `done_fn` is never invoked and the work item is cleaned up via the done queue

#### Scenario: Cancel while task is running
- **WHEN** `xWorkCancel` is called on a work item whose task is already executing
- **THEN** `done_fn` is never invoked when the worker completes and pushes the item to the done queue

### Requirement: xWorkCancel always returns xErrno_Ok

The system SHALL return `xErrno_Ok` from `xWorkCancel` when the cancelled flag has been successfully set and the caller can safely release resources.

#### Scenario: Successful cancel
- **WHEN** `xWorkCancel` is called on any work item (running or pending)
- **THEN** the function returns `xErrno_Ok` (except for NULL argument which returns `xErrno_InvalidArg`)
