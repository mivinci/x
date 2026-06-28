## 1. API changes

- [ ] 1.1 Add `xWorkCancelFunc` typedef to `event.h`
- [ ] 1.2 Add `on_cancel` parameter to `xWorkSubmit` declaration in `event.h`

## 2. Implementation

- [ ] 2.1 Add `on_cancel` field to `struct xWork_` in `event_private.h`
- [ ] 2.2 Store `on_cancel` in `xWorkSubmit` in `event_offload.c`
- [ ] 2.3 Invoke `on_cancel` in `loop_run_done` when cancelled in `event_private.c`

## 3. Update callers

- [ ] 3.1 Update all `xWorkSubmit` call sites to pass NULL as on_cancel
- [ ] 3.2 Simplify `dns.c`: add `dns_cleanup`, remove `req->cancelled`, simplify `dns_done_fn`

## 4. Tests

- [ ] 4.1 Test on_cancel fires for cancelled work
- [ ] 4.2 Test on_cancel does NOT fire for non-cancelled work
- [ ] 4.3 Test on_cancel with NULL (backward compat)

## 5. Build and verify

- [ ] 5.1 Build with cmake, fix compilation errors
- [ ] 5.2 Run full test suite, verify no regressions
