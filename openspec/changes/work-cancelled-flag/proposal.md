## Why

`xWorkCancel` has a safety gap: when the task is already running, `done_fn` still fires. The caller can't reliably release resources after cancel — they must keep `arg` alive until the callback fires, defeating the purpose of cancellation. The current comment claims done_fn won't fire, but `loop_run_done` unconditionally calls it — code and documentation are inconsistent.

Additionally, `xTaskCancel` returns `xErrno_InvalidState` when the task is already running. This is misleading — `InvalidState` implies misuse, not "task in progress." A dedicated `xErrno_InProgress` makes the semantics clear and lets callers distinguish "already running" from actual errors.

## What Changes

- Add `cancelled` field to `struct xWork_`
- Add `xErrno_InProgress` to error codes
- `xTaskCancel` returns `xErrno_InProgress` instead of `xErrno_InvalidState` when task is running
- `xWorkCancel` sets `cancelled = 1`, then tries `xTaskCancel`. Always returns `xErrno_Ok`.
- `loop_run_done` checks `w->cancelled` and skips `done_fn` if set
- Update event API doc for `xWorkCancel` return value

## Capabilities

### Modified Capabilities

<!-- None — no existing specs -->

## Impact

- `libx/x/base/error.h` — add `xErrno_InProgress`
- `libx/x/base/event_private.h` — add `cancelled` to `struct xWork_`
- `libx/x/base/event_offload.c` — `xWorkCancel` logic
- `libx/x/base/event_run.c` — `loop_run_done` check
- `libx/x/base/task.c` — return `xErrno_InProgress` instead of `xErrno_InvalidState`
- `libx/x/base/event.h` — update `xWorkCancel` doc
