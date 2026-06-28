## Context

`xWorkCancel` currently depends on `xTaskCancel` to stop pending work. When the task is dequeued or running, cancel fails and `done_fn` fires normally. The work item cleanup path (`loop_run_done`) always calls `done_fn` unconditionally.

## Goals / Non-Goals

**Goals:**
- After `xWorkCancel`, `done_fn` SHALL NOT be called regardless of task state
- `xTaskCancel` SHALL return a distinct error code (`xErrno_InProgress`) when task is already running
- Backward compatible for callers who handle `done_fn` already being called

**Non-Goals:**
- Changing `xTaskCancel` to block or wait
- Thread-safe `cancelled` flag (only set from event loop thread per current contract)

## Decisions

### 1. `cancelled` flag on `xWork_`

```c
struct xWork_ {
  ...
  int cancelled;  // set by xWorkCancel, checked by loop_run_done
};
```

Set to 1 in `xWorkCancel` before any other operation. `loop_run_done` checks it before calling `done_fn`. No atomic needed — `xWorkCancel` is called from the event loop thread per current contract.

### 2. `xWorkCancel` always returns `xErrno_Ok`

Previous return values (`xErrno_Busy`, `xErrno_InvalidArg`) become unnecessary:
- `xErrno_Busy` — was returned when task already running. Now handled by the `cancelled` flag.
- `xErrno_InvalidArg` — only for NULL work, can still apply.

New behavior:
```
xWorkCancel(w):
  if (!w) return xErrno_InvalidArg;
  w->cancelled = 1;
  err = xTaskCancel(w->task);
  if (err == xErrno_Ok) {
    // Task was pending, now dead. Push to done queue for cleanup.
    w->result = NULL;
    xMpscPush(&done_head, &done_tail, &w->mpsc);
    xEventLoopWake(w->loop);
  }
  // else: task is running — worker will push to done queue,
  // loop_run_done will see cancelled and skip done_fn.
  return xErrno_Ok;
```

### 3. `xErrno_InProgress` for task cancellation

```c
// error.h
XDEF_ENUM(xErrno) {
  ...
  xErrno_InProgress,   // Operation is already in progress
};
```

`xTaskCancel` returns `xErrno_InProgress` when the task is dequeued or executing. This replaces `xErrno_InvalidState` for this specific case.

### 4. `loop_run_done` check

```c
while ((w = xMpscPop(&loop->done_head))) {
  if (w->cancelled) goto free_it;
  if (w->done_fn) w->done_fn(w->arg, w->result);
free_it:
  event_work_free(loop, w);
}
```

## Risks / Trade-offs

- Callers that previously relied on `done_fn` always firing (even after cancel) will see different behavior. Mitigation: search for callers that handle `xErrno_Busy` from `xWorkCancel` and update them.
- `xErrno_InProgress` change to `xTaskCancel` may affect other callers of `xTaskCancel`. Mitigation: replace all instances of `xErrno_InvalidState` usage with `xErrno_InProgress` where task state is the cause.
