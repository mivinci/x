# async-fs

## ADDED Requirements

### Requirement: Unified request submission

The system SHALL provide `xFsReqSubmit(xFsReq *req)` for all async filesystem operations. The operation type is specified by `req->op`. Results are delivered via `req->cb` on the event loop thread, or synchronously when `cb` is NULL.

#### Scenario: Async open
- **WHEN** `xFsReqSubmit` is called with `op = xFsOpOpen`, a path, flags, and a callback
- **THEN** the file is opened on a worker thread and `cb` fires on the event loop thread with `out_file` set

#### Scenario: Sync stat
- **WHEN** `xFsReqSubmit` is called with `op = xFsOpStat` and `cb = NULL`
- **THEN** the call blocks until `stat` is populated, then returns

### Requirement: Streaming read with done flag

`xFsOpRead` SHALL invoke the callback multiple times with chunks of data, setting `done = true` on the final invocation.

#### Scenario: Multi-chunk read
- **WHEN** a large file is read with `len = SIZE_MAX`
- **THEN** the callback is invoked one or more times with `retval` bytes per chunk, and `done = true` on the last call

### Requirement: Cancel

`xFsReqCancel(req)` SHALL prevent the callback from firing and free no heap resources (all buffers are caller-owned).

#### Scenario: Cancel pending operation
- **WHEN** `xFsReqCancel` is called on a pending or running request
- **THEN** `cb` is never invoked
