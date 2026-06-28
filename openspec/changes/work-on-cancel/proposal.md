## Why

`xWorkCancel` now prevents `done_fn` from firing, but when work was already running, the worker-allocated result leaks. The on_cancel callback solves this: it fires in `loop_run_done` when cancelled, giving the caller a hook to free heap-allocated results and associated resources.

## What Changes

- Add `on_cancel` parameter to `xWorkSubmit` (NULL for backward-compat)
- Store `on_cancel` in `struct xWork_`
- `loop_run_done` calls `on_cancel` when `w->cancelled` is set
- Simplify `xDnsCancel` — remove `req->cancelled` self-managed cleanup, delegate to on_cancel
- Simplify `dns_done_fn` — remove cancelled flag check

## Capabilities

### Modified Capabilities

<!-- None — internal API enhancement -->

## Impact

- `libx/x/base/event.h` — add `on_cancel` param to `xWorkSubmit`
- `libx/x/base/event_private.h` — add `on_cancel` to `struct xWork_`
- `libx/x/base/event_private.c` — `loop_run_done` invokes `on_cancel`
- `libx/x/base/event_offload.c` — `xWorkSubmit` stores `on_cancel`
- `libx/x/net/dns.c` — simplify `xDnsCancel` and `dns_done_fn`
- All `xWorkSubmit` callers — add NULL as last arg (backward-compat)
