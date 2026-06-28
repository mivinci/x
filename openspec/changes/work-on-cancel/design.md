## Context

`xWorkCancel` (from work-cancelled-flag change) guarantees done_fn won't fire through the cancelled flag. This creates a resource leak for callers whose work_fn allocates heap objects (like `dns_work_fn` returning a `xDnsResult*`). The solution: a dedicated cleanup callback invoked by `loop_run_done` for cancelled work.

## Goals / Non-Goals

**Goals:**
- Provide hook for work-specific resource cleanup after cancel
- Simplify `xDnsCancel` / `dns_done_fn` by removing self-managed cancelled flag
- Backward compatible: `on_cancel = NULL` preserves existing behavior

**Non-Goals:**
- Changing the signature of `xWorkSubmit` for existing callers (just adds one param)

## Decisions

### 1. Add `on_cancel` to `xWorkSubmit`

```c
xWork xWorkSubmit(xTaskGroup group, xTaskFunc work_fn,
                   xWorkDoneFunc done_fn,
                   xWorkCancelFunc on_cancel,  // NEW, NULL = no-op
                   void *arg);
```

### 2. `loop_run_done` invokes `on_cancel`

```c
if (w->cancelled) {
    if (w->on_cancel) w->on_cancel(w->arg, w->result);
    goto free_it;
}
```

### 3. DNS cleanup via on_cancel

```c
static void dns_cleanup(void *arg, void *result) {
    struct xDnsRequest_ *req = arg;
    if (result) xDnsResultFree(result);
    free(req->hostname);
    free(req->service);
    free(req);
}

// Submit
req->work = xWorkSubmit(NULL, dns_work_fn, dns_done_fn, dns_cleanup, req);
```

`xDnsCancel` becomes one-liner, `dns_done_fn` removes cancelled check.

## Risks / Trade-offs

- All existing `xWorkSubmit` callers must add `NULL` as the on_cancel argument. One-time mechanical change.
