# work-on-cancel

## ADDED Requirements

### Requirement: on_cancel callback

`xWorkSubmit` SHALL accept a `xWorkCancelFunc on_cancel` parameter. When non-NULL, this callback SHALL be invoked by `loop_run_done` for work items that were cancelled, passing the work's `arg` and `result` — but SHALL only be used to free worker-allocated `result` data, NOT to free `arg` (which is owned by the caller and freed synchronously after `xWorkCancel` returns).

#### Scenario: Cancelled work triggers result cleanup
- **WHEN** a work item with `on_cancel` set is cancelled via `xWorkCancel`
- **THEN** `loop_run_done` invokes `on_cancel(w->arg, w->result)` and skips `done_fn`

#### Scenario: on_cancel only frees result, not arg
- **WHEN** `on_cancel` fires
- **THEN** it SHALL free `result` (if any) but SHALL NOT free `arg` or its associated members

#### Scenario: Non-cancelled work ignores on_cancel
- **WHEN** a work item completes normally (not cancelled)
- **THEN** `on_cancel` is never invoked, and `done_fn` fires as usual

#### Scenario: NULL on_cancel is backward-compatible
- **WHEN** a work item is submitted with `on_cancel = NULL`
- **THEN** cancelled work is silently freed without any cleanup callback

### Requirement: Simplified DNS cancel

`xDnsCancel` SHALL free request resources (hostname, service, req) synchronously after `xWorkCancel`, and delegate worker-allocated result cleanup to `on_cancel`.

#### Scenario: DNS cancel frees resources
- **WHEN** `xDnsCancel` is called on a pending or running DNS request
- **THEN** hostname, service, and req are freed synchronously; worker-allocated xDnsResult is freed by `on_cancel` in `loop_run_done`
